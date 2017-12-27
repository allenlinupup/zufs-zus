/*
 * toyfs.c - The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <linux/falloc.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"
#include "toyfs-utils.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x_)	(sizeof(x_)/sizeof(x_[0]))
#endif

#define Z2SBI(zsbi) _zsbi_to_sbi(zsbi)
#define Z2II(zii) _zii_to_tii(zii)


static const struct zus_zii_operations toyfs_zii_op;
static const struct zus_zfi_operations toyfs_zfi_op;
static const struct zus_sbi_operations toyfs_sbi_op;
static int toyfs_truncate(struct toyfs_inode_info *tii, size_t size);
static struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi);
static size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr);
static void _drop_blkref(struct toyfs_inode_info *, struct toyfs_blkref *);


/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-common.[ch] */

TOYFS_NORETURN
static void toyfs_panicf(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	FILE *fp = stderr;

	flockfile(fp);
	fputs("toyfs: ", fp);
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fprintf(fp, " (%s:%d)\n", file, line);
	funlockfile(fp);
	abort();
}

static void _mutex_init(pthread_mutex_t *mutex)
{
	int err;
	pthread_mutexattr_t attr;

	err = pthread_mutexattr_init(&attr);
	toyfs_panic_if_err(err, "pthread_mutexattr_init");

	err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	toyfs_panic_if_err(err, "pthread_mutexattr_settype");

	err = pthread_mutex_init(mutex, &attr);
	toyfs_panic_if_err(err, "pthread_mutex_init");

	err = pthread_mutexattr_destroy(&attr);
	toyfs_panic_if_err(err, "pthread_mutexattr_destroy");
}

static void _mutex_destroy(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_destroy(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_destroy");
}

static void _mutex_lock(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_lock(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_lock");
}

static void _mutex_unlock(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_unlock(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_unlock");
}

static void list_add_before(struct list_head *elem, struct list_head *head)
{
	_list_add(elem, head->prev, head);
}

static mode_t toyfs_mode_of(const struct toyfs_inode_info *tii)
{
	return tii->ti->zi.i_mode;
}

static struct toyfs_inode_info *_zii_to_tii(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = NULL;

	if (zii) {
		tii = container_of(zii, struct toyfs_inode_info, zii);
		toyfs_assert(tii->imagic == TOYFS_IMAGIC);
	}
	return tii;
}


/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-super.c */

#define TOYFS_INODES_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(union toyfs_inode_head))

#define TOYFS_BLKREFS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_blkref))

#define TOYFS_DIRENTS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_dirent))

union toyfs_pool_page {
	struct toyfs_page page;
	union toyfs_pool_page *next;
};

union toyfs_inodes_page {
	struct toyfs_page page;
	union toyfs_inode_head inodes[TOYFS_INODES_PER_PAGE];
};

union toyfs_blkrefs_page {
	struct toyfs_page page;
	struct toyfs_blkref blkrefs[TOYFS_BLKREFS_PER_PAGE];
};

union toyfs_dirents_page {
	struct toyfs_page page;
	struct toyfs_dirent dirents[TOYFS_DIRENTS_PER_PAGE];
};

static int _mmap_memory(size_t msz, void **pp)
{
	int err = 0;
	void *mem;
	const int prot = PROT_WRITE | PROT_READ;
	const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (msz < PAGE_SIZE)
		return -EINVAL;

	mem = mmap(NULL, msz, prot, flags, -1, 0);
	if (mem != MAP_FAILED) {
		*pp = mem;
		toyfs_info("mmap ok: %p", mem);
	} else {
		err = -errno;
		toyfs_info("mmap failed: %d", err);
	}
	return err;
}

static void _munmap_memory(void *mem, size_t msz)
{
	if (mem) {
		toyfs_info("munmap %p %lu", mem, msz);
		munmap(mem, msz);
	}
}

static void _pool_init(struct toyfs_pool *pool)
{
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	list_init(&pool->bfree);
	list_init(&pool->dfree);
	list_init(&pool->ifree);
	_mutex_init(&pool->mutex);
}

static void
_pool_setup(struct toyfs_pool *pool, void *mem, size_t msz, bool pmem)
{
	size_t npages, pagei;
	union toyfs_pool_page *page, *next;
	union toyfs_pool_page *pages_arr = mem;

	page = next = NULL;
	npages = msz / sizeof(*page);
	for (pagei = 0; pagei < npages; ++pagei) {
		page = &pages_arr[pagei];
		page->next = next;
		next = page;
	}
	pool->mem = mem;
	pool->msz = msz;
	pool->pages = page;
	pool->pmem = pmem;
}

static void _pool_destroy(struct toyfs_pool *pool)
{
	if (pool->mem && !pool->pmem)
		_munmap_memory(pool->mem, pool->msz);
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	_mutex_destroy(&pool->mutex);
}

static void _pool_lock(struct toyfs_pool *pool)
{
	_mutex_lock(&pool->mutex);
}

static void _pool_unlock(struct toyfs_pool *pool)
{
	_mutex_unlock(&pool->mutex);
}

static struct toyfs_page *_pool_pop_page_without_lock(struct toyfs_pool *pool)
{
	struct toyfs_page *page = NULL;
	union toyfs_pool_page *ppage;

	if (pool->pages) {
		ppage = pool->pages;
		pool->pages = ppage->next;
		ppage->next = NULL;
		page = &ppage->page;
	}
	return page;
}

static struct toyfs_page *_pool_pop_page(struct toyfs_pool *pool)
{
	struct toyfs_page *page;

	_pool_lock(pool);
	page = _pool_pop_page_without_lock(pool);
	_pool_unlock(pool);
	return page;
}

static void _pool_push_page(struct toyfs_pool *pool, struct toyfs_page *page)
{
	union toyfs_pool_page *ppage;

	_pool_lock(pool);
	ppage = container_of(page, union toyfs_pool_page, page);
	ppage->next = pool->pages;
	pool->pages = ppage;
	_pool_unlock(pool);
}

static int _pool_add_ifree(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_inodes_page *ipage;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	ipage = (union toyfs_inodes_page *)page;
	for (i = 0; i < ARRAY_SIZE(ipage->inodes); ++i)
		list_add(&ipage->inodes[i].head, &pool->ifree);

	return 0;
}

static struct toyfs_inode *_list_head_to_inode(struct list_head *head)
{
	union toyfs_inode_head *ihead;

	ihead = container_of(head, union toyfs_inode_head, head);
	return &ihead->inode;
}

static struct toyfs_inode *_pool_pop_ifree(struct toyfs_pool *pool)
{
	struct toyfs_inode *ti = NULL;

	if (!list_empty(&pool->ifree)) {
		ti = _list_head_to_inode(pool->ifree.next);
		list_del(pool->ifree.next);
	}
	return ti;
}

static struct toyfs_inode *_pool_pop_inode(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_inode *ti;

	_pool_lock(pool);
	ti = _pool_pop_ifree(pool);
	if (ti)
		goto out;

	err = _pool_add_ifree(pool);
	if (err)
		goto out;

	ti = _pool_pop_ifree(pool);
out:
	_pool_unlock(pool);
	return ti;
}

static void _pool_push_inode(struct toyfs_pool *pool, struct toyfs_inode *inode)
{
	union toyfs_inode_head *ihead;

	ihead = container_of(inode, union toyfs_inode_head, inode);
	memset(ihead, 0, sizeof(*ihead));

	_pool_lock(pool);
	list_add_tail(&ihead->head, &pool->ifree);
	_pool_unlock(pool);
}

static int _pool_add_dfree(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_dirents_page *dpage;
	struct toyfs_dirent *dirent;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	dpage = (union toyfs_dirents_page *)page;
	for (i = 0; i < ARRAY_SIZE(dpage->dirents); ++i) {
		dirent = &dpage->dirents[i];
		list_add_tail(&dirent->d_head, &pool->dfree);
	}
	return 0;
}

static struct toyfs_dirent *_pool_pop_dfree(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_dirent *dirent = NULL;

	if (!list_empty(&pool->dfree)) {
		elem = pool->dfree.next;
		list_del(elem);
		dirent = container_of(elem, struct toyfs_dirent, d_head);
	}
	return dirent;
}

static struct toyfs_dirent *_pool_pop_dirent(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_dirent *dirent;

	_pool_lock(pool);
	dirent = _pool_pop_dfree(pool);
	if (!dirent) {
		err = _pool_add_dfree(pool);
		if (!err)
			dirent = _pool_pop_dfree(pool);
	}
	_pool_unlock(pool);
	return dirent;
}


static int _pool_add_bfree(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_blkrefs_page *bpage;
	struct toyfs_blkref *blkref;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	bpage = (union toyfs_blkrefs_page *)page;
	for (i = 0; i < ARRAY_SIZE(bpage->blkrefs); ++i) {
		blkref = &bpage->blkrefs[i];
		list_add_tail(&blkref->b_head, &pool->bfree);
	}
	return 0;
}

static struct toyfs_blkref *_pool_pop_bfree(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_blkref *blkref = NULL;

	if (!list_empty(&pool->bfree)) {
		elem = pool->bfree.next;
		list_del(elem);
		blkref = container_of(elem, struct toyfs_blkref, b_head);
	}
	return blkref;
}

static struct toyfs_blkref *_pool_pop_blkref(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_blkref *blkref;

	_pool_lock(pool);
	blkref = _pool_pop_bfree(pool);
	if (!blkref) {
		err = _pool_add_bfree(pool);
		if (!err)
			blkref = _pool_pop_bfree(pool);
	}
	_pool_unlock(pool);
	return blkref;
}

static void _pool_push_blkref(struct toyfs_pool *pool,
			      struct toyfs_blkref *blkref)
{
	_pool_lock(pool);
	list_add(&blkref->b_head, &pool->bfree);
	_pool_unlock(pool);
}

static void _itable_init(struct toyfs_itable *itable)
{
	itable->icount = 0;
	memset(itable->imap, 0x00, sizeof(itable->imap));
	_mutex_init(&itable->mutex);
}

static void _itable_destroy(struct toyfs_itable *itable)
{
	itable->icount = 0;
	memset(itable->imap, 0xff, sizeof(itable->imap));
	_mutex_destroy(&itable->mutex);
}

static void _itable_lock(struct toyfs_itable *itable)
{
	_mutex_lock(&itable->mutex);
}

static void _itable_unlock(struct toyfs_itable *itable)
{
	_mutex_unlock(&itable->mutex);
}

static size_t _itable_slot_of(const struct toyfs_itable *itable, ino_t ino)
{
	return ino % ARRAY_SIZE(itable->imap);
}

static struct toyfs_inode_info *
_itable_find(struct toyfs_itable *itable, ino_t ino)
{
	size_t slot;
	struct toyfs_inode_info *tii;

	_itable_lock(itable);
	slot = _itable_slot_of(itable, ino);
	tii = itable->imap[slot];
	while (tii != NULL) {
		if (tii->ino == ino)
			break;
		tii = tii->next;
	}
	_itable_unlock(itable);
	return tii;
}

static void _itable_insert(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_info **ient;

	toyfs_assert(tii->ti);
	toyfs_assert(tii->sbi);
	toyfs_assert(!tii->next);

	_itable_lock(itable);
	slot = _itable_slot_of(itable, tii->ino);
	ient = &itable->imap[slot];
	tii->next = *ient;
	*ient = tii;
	itable->icount++;
	_itable_unlock(itable);
}

static void _itable_remove(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_info **ient;

	_itable_lock(itable);
	toyfs_assert(itable->icount > 0);
	slot = _itable_slot_of(itable, tii->ino);
	ient = &itable->imap[slot];
	toyfs_assert(*ient != NULL);
	while (*ient) {
		if (*ient == tii)
			break;
		ient = &(*ient)->next;
	}
	toyfs_assert(*ient != NULL);
	*ient = tii->next;
	itable->icount--;
	_itable_unlock(itable);

	tii->next = NULL;
}


static void _sbi_lock(struct toyfs_sb_info *sbi)
{
	_mutex_lock(&sbi->s_mutex);
}

static void _sbi_unlock(struct toyfs_sb_info *sbi)
{
	_mutex_unlock(&sbi->s_mutex);
}

static struct zus_sb_info *toyfs_sbi_alloc(struct zus_fs_info *zfi)
{
	struct toyfs_sb_info *sbi;

	toyfs_info("sbi_alloc: zfi=%p", zfi);

	sbi = (struct toyfs_sb_info *)malloc(sizeof(*sbi));
	if (!sbi)
		return NULL;

	memset(sbi, 0, sizeof(*sbi));
	_mutex_init(&sbi->s_mutex);
	_pool_init(&sbi->s_pool);
	_itable_init(&sbi->s_itable);
	sbi->s_zus_sbi.op = &toyfs_sbi_op;
	sbi->s_zus_sbi.pmem.user_page_size = PAGE_SIZE;
	return &sbi->s_zus_sbi;
}

static struct toyfs_sb_info *_zsbi_to_sbi(struct zus_sb_info *zsbi)
{
	return container_of(zsbi, struct toyfs_sb_info, s_zus_sbi);
}

static void toyfs_sbi_free(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	toyfs_info("sbi_free: zsbi=%p", zsbi);
	free(sbi);
}

static struct toyfs_page *toyfs_alloc_page(struct toyfs_sb_info *sbi)
{
	struct toyfs_page *page = NULL;

	/* TODO: Distinguish between user types */
	_sbi_lock(sbi);
	if (!sbi->s_statvfs.f_bfree)
		goto out;
	if (!sbi->s_statvfs.f_bavail)
		goto out;
	page = _pool_pop_page(&sbi->s_pool);
	if (!page)
		goto out;

	memset(page, 0x00, sizeof(*page));
	sbi->s_statvfs.f_bfree--;
	sbi->s_statvfs.f_bavail--;
	toyfs_info("alloc_page: blocks=%lu bfree=%lu pmem_bn=%lu",
		   sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
		   toyfs_addr2bn(sbi, page));
out:
	_sbi_unlock(sbi);
	return page;
}

static void toyfs_free_page(struct toyfs_sb_info *sbi, struct toyfs_page *page)
{
	_sbi_lock(sbi);
	_pool_push_page(&sbi->s_pool, page);
	sbi->s_statvfs.f_bfree++;
	sbi->s_statvfs.f_bavail++;
	toyfs_info("free_page: blocks=%lu bfree=%lu pmem_bn=%lu",
		   sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
		   toyfs_addr2bn(sbi, page));
	_sbi_unlock(sbi);
}

static struct toyfs_blkref *toyfs_alloc_blkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_blkref *blkref;

	blkref = _pool_pop_blkref(&sbi->s_pool);
	if (blkref) {
		blkref->b_off = -1;
		blkref->b_bn = 0;
	}
	return blkref;
}

static void toyfs_free_blkref(struct toyfs_sb_info *sbi,
			      struct toyfs_blkref *blkref)
{
	_pool_push_blkref(&sbi->s_pool, blkref);
}

static struct toyfs_dirent *toyfs_alloc_dirent(struct toyfs_sb_info *sbi)
{
	return _pool_pop_dirent(&sbi->s_pool);
}

static void toyfs_sbi_setup(struct toyfs_sb_info *sbi)
{
	/* TODO: FIXME */
	const size_t fssize = sbi->s_pool.msz;
	const size_t fssize_blocks = fssize / PAGE_SIZE;

	sbi->s_top_ino = TOYFS_ROOT_INO + 1;
	sbi->s_statvfs.f_bsize = PAGE_SIZE;
	sbi->s_statvfs.f_frsize = PAGE_SIZE;
	sbi->s_statvfs.f_blocks = fssize / PAGE_SIZE;
	sbi->s_statvfs.f_bfree = fssize_blocks;
	sbi->s_statvfs.f_bavail = fssize_blocks;
	sbi->s_statvfs.f_files = fssize_blocks;
	sbi->s_statvfs.f_ffree = fssize_blocks;
	sbi->s_statvfs.f_favail = fssize_blocks;
	sbi->s_statvfs.f_namemax = TOYFS_NAME_MAX;
}

static int toyfs_new_root_inode(struct toyfs_sb_info *sbi,
				struct toyfs_inode_info **out_ii)
{
	struct toyfs_inode *root_ti;
	struct toyfs_inode_info *root_tii;

	root_tii = toyfs_alloc_ii(sbi);
	if (!root_tii)
		return -ENOMEM;

	root_ti = _pool_pop_inode(&sbi->s_pool);
	if (!root_ti)
		return -ENOSPC;

	memset(root_ti, 0, sizeof(*root_ti));
	root_tii->ti = root_ti;
	root_tii->zii.zi = &root_ti->zi;
	root_tii->ino = TOYFS_ROOT_INO;

	root_ti = root_tii->ti;
	root_ti->zi.i_ino = TOYFS_ROOT_INO;
	root_ti->zi.i_mode = 0755 | S_IFDIR;
	root_ti->zi.i_nlink = 2;
	root_ti->zi.i_uid = 0;
	root_ti->zi.i_gid = 0;
	root_ti->zi.i_generation = 0;
	root_ti->zi.i_rdev = 0;
	root_ti->zi.i_size = 0;
	/* TODO: FIXME
	root_i->ti_zi.i_blksize = TOYFS_PAGE_SIZE;
	root_i->ti_zi.i_blocks = 0;
	*/
	root_ti->i_parent_ino = TOYFS_ROOT_INO;
	root_ti->ti.dir.d_ndentry = 0;
	root_ti->ti.dir.d_off_max = 2;
	list_init(&root_ti->ti.dir.d_childs);

	_itable_insert(&sbi->s_itable, root_tii);
	*out_ii = root_tii;
	return 0;
}


static int _read_pmem_sb_first_time(struct zus_pmem *pmem)
{
	void *pmem_addr = pmem->p_pmem_addr;
	const struct toyfs_super_block *sb;

	sb = (const struct toyfs_super_block *)pmem_addr;
	if (sb->part1.dev_table.s_magic != ZUFS_SUPER_MAGIC) {
		toyfs_error("illegal magic1: %ld\n",
			    (long)sb->part1.dev_table.s_magic);
		return -EINVAL;
	}
	if (sb->part2.dev_table.s_magic != ZUFS_SUPER_MAGIC) {
		toyfs_error("illegal magic2: %ld\n",
			    (long)sb->part2.dev_table.s_magic);
		return -EINVAL;
	}
	return 0;
}

static void _read_pmem_first_time(struct zus_pmem *pmem)
{
	size_t i, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = pmem->p_pmem_addr;
	char *ptr;
	char buf[TOYFS_KILO];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = pmem_blocks(pmem);
	pmem_total_size = pmem_p2o(pmem_total_blocks);
	ptr = (char *)pmem_addr;
	for (i = 0; i < pmem_total_size; i += buf_size) {
		memcpy(buf, ptr, buf_size);
		ptr += buf_size;
	}
}

static void _write_pmem_first_time(struct zus_pmem *pmem)
{
	size_t i, head_size, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = pmem->p_pmem_addr;
	char *ptr;
	char buf[TOYFS_KILO];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = pmem_blocks(pmem);
	pmem_total_size = pmem_p2o(pmem_total_blocks);

	head_size = (2 * PAGE_SIZE);
	ptr = (char *)pmem_addr + head_size;
	for (i = head_size; i < pmem_total_size; i += buf_size) {
		memset(buf, i, buf_size);
		memcpy(ptr, buf, buf_size);
		ptr += buf_size;
	}
}

static int _prepare_pmem_first_time(struct zus_pmem *pmem)
{
	int err;

	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	_read_pmem_first_time(pmem);
	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	_write_pmem_first_time(pmem);
	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	return 0;
}

static int toyfs_sbi_init(struct zus_sb_info *zsbi, struct zufs_ioc_mount *zim)
{
	int err;
	void *mem = NULL;
	size_t pmem_total_blocks, msz = 0;
	uint32_t pmem_kernel_id;
	bool using_pmem = false;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	toyfs_info("sbi_init: zsbi=%p", zsbi);

	pmem_kernel_id = sbi->s_zus_sbi.pmem.pmem_info.pmem_kern_id;
	pmem_total_blocks = pmem_blocks(&sbi->s_zus_sbi.pmem);
	if ((pmem_kernel_id > 0) && (pmem_total_blocks > 2)) {
		err = _prepare_pmem_first_time(&zsbi->pmem);
		if (err)
			return err;

		msz = pmem_p2o(pmem_total_blocks - 2);
		mem = pmem_baddr(&sbi->s_zus_sbi.pmem, 2);
		using_pmem = true;
	} else {
		msz = (1ULL << 30) /* 1G */;
		err = _mmap_memory(msz, &mem);
		if (err)
			return err;
	}

	_pool_setup(&sbi->s_pool, mem, msz, using_pmem);
	toyfs_sbi_setup(sbi);

	/* TODO: Take root inode from super */

	err = toyfs_new_root_inode(sbi, &sbi->s_root);
	if (err)
		return err;

	zsbi->z_root = &sbi->s_root->zii;
	zim->zus_sbi = zsbi;
	zim->zus_ii = zsbi->z_root;
	zim->s_blocksize_bits  = TOYFS_PAGE_SHIFT;

	return 0;
}

static int toyfs_sbi_fini(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	toyfs_info("sbi_fini: zsbi=%p", zsbi);

	_pool_destroy(&sbi->s_pool);
	_itable_destroy(&sbi->s_itable);
	_mutex_destroy(&sbi->s_mutex);
	sbi->s_root = NULL;
	return 0;
}

static size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr)
{
	size_t offset;
	struct zus_pmem *pmem = &sbi->s_zus_sbi.pmem;

	offset = pmem_addr_2_offset(pmem, ptr);
	return pmem_o2p(offset);
}

static void *toyfs_bn2addr(struct toyfs_sb_info *sbi, size_t bn)
{
	struct zus_pmem *pmem = &sbi->s_zus_sbi.pmem;

	return pmem_baddr(pmem, bn);
}

static struct toyfs_page *toyfs_bn2page(struct toyfs_sb_info *sbi, size_t bn)
{
	return (struct toyfs_page *)toyfs_bn2addr(sbi, bn);
}

static struct toyfs_inode_info *
toyfs_find_inode(struct toyfs_sb_info *sbi, ino_t ino)
{
	toyfs_info("find_inode: ino=%lu", ino);
	return _itable_find(&sbi->s_itable, ino);
}

static int
toyfs_iget(struct zus_sb_info *zsbi, struct zus_inode_info *zii, ulong ino)
{
	int err = 0;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii;

	toyfs_info("iget: ino=%lu", ino);

	toyfs_assert(zii->op);
	tii = toyfs_find_inode(sbi, ino);
	if (tii) {
		zii->zi = tii->zii.zi;
		toyfs_info("iget: ino=%lu zi=%p", ino, zii->zi);
	} else {
		err = -ENOENT;
		toyfs_info("iget: ino=%lu err=%d", ino, err);
	}
	return err;
}

static struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi)
{
	struct toyfs_inode_info *tii = NULL;

	/* TODO: Distinguish between user types */
	if (!sbi->s_statvfs.f_ffree || !sbi->s_statvfs.f_favail)
		return NULL;

	tii = (struct toyfs_inode_info *)malloc(sizeof(*tii));
	if (!tii)
		return NULL;

	memset(tii, 0, sizeof(*tii));
	tii->imagic = TOYFS_IMAGIC;
	tii->next = NULL;
	tii->sbi = sbi;
	tii->zii.op = &toyfs_zii_op;
	tii->zii.sbi = &sbi->s_zus_sbi;

	sbi->s_statvfs.f_ffree--;
	sbi->s_statvfs.f_favail--;

	toyfs_info("alloc_ii tii=%p files=%lu ffree=%lu", tii,
		   sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);
	return tii;
}

static void toyfs_free_ii(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;

	memset(tii, 0xAB, sizeof(*tii));
	tii->zii.op = NULL;
	tii->sbi = NULL;
	free(tii);
	sbi->s_statvfs.f_ffree++;
	sbi->s_statvfs.f_favail++;
	toyfs_info("free_ii tii=%p files=%lu ffree=%lu", tii,
		   sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);
}

static struct zus_inode_info *toyfs_zii_alloc(struct zus_sb_info *zsbi)
{
	struct toyfs_inode_info *tii;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	_sbi_lock(sbi);
	tii = toyfs_alloc_ii(sbi);
	_sbi_unlock(sbi);

	toyfs_info("zii_alloc: zii=%p", &tii->zii);

	return tii ? &tii->zii : NULL;
}

static void toyfs_zii_free(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;

	toyfs_info("zii_free: zii=%p", zii);

	_sbi_lock(sbi);
	toyfs_free_ii(tii);
	_sbi_unlock(sbi);
}

static ino_t toyfs_next_ino(struct toyfs_sb_info *sbi)
{
	return __atomic_fetch_add(&sbi->s_top_ino, 1, __ATOMIC_CONSUME);
}

static int toyfs_statfs(struct zus_sb_info *zsbi,
			struct zufs_ioc_statfs *ioc_statfs)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct statfs64 *out = &ioc_statfs->statfs_out;
	const struct statvfs *stvfs = &sbi->s_statvfs;

	toyfs_info("statfs sbi=%p", sbi);

	_sbi_lock(sbi);
	out->f_bsize = stvfs->f_bsize;
	out->f_blocks = stvfs->f_blocks;
	out->f_bfree = stvfs->f_bfree;
	out->f_bavail = stvfs->f_bavail;
	out->f_files = stvfs->f_files;
	out->f_ffree = stvfs->f_ffree;
	out->f_namelen = stvfs->f_namemax;
	out->f_frsize = stvfs->f_frsize;
	out->f_flags = stvfs->f_flag;
	_sbi_unlock(sbi);

	toyfs_info("statfs: bsize=%ld blocks=%ld bfree=%ld bavail=%ld "
		   "files=%ld ffree=%ld", (long)out->f_bsize,
		   (long)out->f_blocks, (long)out->f_bfree, (long)out->f_bavail,
		   (long)out->f_files, (long)out->f_ffree);

	return 0;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-inode.c */

static const char *toyfs_symlink_value(const struct toyfs_inode_info *tii)
{
	const struct toyfs_inode *ti = tii->ti;
	const struct zus_inode *zi = &ti->zi;
	const char *symlnk = NULL;

	if (zi_islnk(zi)) {
		if (zi->i_size > sizeof(zi->i_symlink))
			symlnk = (const char *)ti->ti.symlnk.sl_long->dat;
		else
			symlnk = (const char *)ti->zi.i_symlink;
	}
	return symlnk;
}

static int toyfs_new_inode(struct zus_sb_info *zsbi, struct zus_inode_info *zii,
			   void *app_ptr, struct zufs_ioc_new_inode *ioc_new)
{
	ino_t ino;
	mode_t mode;
	size_t symlen;
	struct toyfs_inode *ti;
	struct zus_inode *zi = &ioc_new->zi;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_inode_info *dir_tii = Z2II(ioc_new->dir_ii);
	struct toyfs_page *page;
	const char *symname;
	bool symlong;

	mode = zi->i_mode;
	toyfs_info("new_inode: zsbi=%p zii=%p mode=%o", zsbi, zii, mode);

	if (!(zi_isdir(zi) || zi_isreg(zi) || zi_islnk(zi) || S_ISFIFO(mode)))
		return -ENOTSUP;
	if (zi->i_size >= TOYFS_PAGE_SIZE)
		return -EINVAL;

	ti = _pool_pop_inode(&sbi->s_pool);
	if (!ti)
		return -ENOSPC;

	ino = toyfs_next_ino(tii->sbi);
	memset(ti, 0, sizeof(*ti));
	memcpy(&ti->zi, zi, sizeof(ti->zi));
	tii->ti = ti;
	tii->ino = ino;
	tii->zii.zi = &tii->ti->zi;
	ti->i_parent_ino = TOYFS_NULL_INO;
	ti->zi.i_ino = ino;

	if (zi_isdir(zi)) {
		toyfs_info("new_inode(dir): ino=%lu", ino);
		list_init(&ti->ti.dir.d_childs);
		ti->ti.dir.d_ndentry = 0;
		ti->ti.dir.d_off_max = 2;
		ti->zi.i_size = TOYFS_PAGE_SIZE;
		ti->i_parent_ino = dir_tii->zii.zi->i_ino;
		zus_std_new_dir(dir_tii->zii.zi, &ti->zi);
	} else if (zi_isreg(zi)) {
		toyfs_info("new_inode(reg): ino=%lu", ino);
		list_init(&ti->ti.reg.r_blkrefs);
		ti->ti.reg.r_first_parent = dir_tii->zii.zi->i_ino;
		if (ioc_new->flags & ZI_TMPFILE)
			ti->zi.i_nlink = 1;
	} else if (zi_islnk(zi)) {
		symlen = ti->zi.i_size;
		symlong = symlen > sizeof(ti->zi.i_symlink);
		symname = symlong ? (const char *)app_ptr :
			  (const char *)zi->i_symlink;
		toyfs_info("new_inode(symlnk): ino=%lu lnk=%.*s",
			   ino, (int)symlen, symname);
		if (symlong) {
			page = toyfs_alloc_page(sbi);
			if (!page) {
				_pool_push_inode(&sbi->s_pool, ti);
				return -ENOSPC;
			}
			memcpy(page->dat, app_ptr, symlen);
			ti->ti.symlnk.sl_long = page;
		}
	} else if (S_ISFIFO(mode)) {
		toyfs_info("new_inode(fifo): ino=%lu", ino);
		ti->ti.reg.r_first_parent = dir_tii->zii.zi->i_ino;
	}


	_itable_insert(&sbi->s_itable, tii);
	ioc_new->zi.i_ino = ino;
	return 0;
}

static void toyfs_release_symlink(struct toyfs_inode_info *tii)
{
	struct toyfs_inode *ti = tii->ti;
	const size_t symlen = ti->zi.i_size;
	struct toyfs_page *page;

	if (symlen > sizeof(ti->zi.i_symlink)) {
		page = ti->ti.symlnk.sl_long;
		toyfs_free_page(tii->sbi, page);
		ti->ti.symlnk.sl_long = NULL;
	}
	ti->zi.i_size = 0;
}

static int toyfs_free_inode(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;
	struct zus_inode *zi = tii->zii.zi;

	toyfs_info("free_inode: ino=%lu mode=%o nlink=%ld size=%ld",
		   tii->ino, (int)zi->i_mode,
		   (long)zi->i_nlink, (long)zi->i_size);

	if (zi_isdir(zi)) {
		toyfs_info("free_inode(dir): ino=%lu", tii->ino);
		if (tii->ti->ti.dir.d_ndentry)
			return -ENOTEMPTY;
		zi->i_dir.parent = 0; /* TODO: Maybe zus_std helper ? */
	} else if (zi_islnk(zi)) {
		toyfs_info("free_inode(symlink): ino=%lu symlnk=%s",
			   tii->ino, toyfs_symlink_value(tii));
		toyfs_release_symlink(tii);
	} else if (zi_isreg(zi)) {
		toyfs_info("free_inode(reg): ino=%lu", tii->ino);
		toyfs_truncate(tii, 0);
	} else {
		toyfs_info("free_inode: ino=%lu mode=%o", tii->ino, zi->i_mode);
		zi->i_rdev = 0;
	}

	_sbi_lock(sbi);
	_itable_remove(&sbi->s_itable, tii);
	_pool_push_inode(&sbi->s_pool, tii->ti);
	_sbi_unlock(sbi);
	return 0;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-dir.c */

static void _set_dirent(struct toyfs_dirent *dirent,
			const char *name, size_t nlen,
			const struct toyfs_inode_info *tii, loff_t off)
{
	toyfs_assert(nlen < sizeof(dirent->d_name));

	memset(dirent, 0, sizeof(*dirent)); /* TODO: rm */
	list_init(&dirent->d_head);
	dirent->d_nlen = nlen;
	dirent->d_ino = tii->ino;
	dirent->d_type = IFTODT(toyfs_mode_of(tii));
	dirent->d_off = off;
	strncpy(dirent->d_name, name, nlen);
}

static loff_t toyfs_next_doff(struct toyfs_inode_info *dir_tii)
{
	loff_t off, *d_off_max = &dir_tii->ti->ti.dir.d_off_max;

	off = __atomic_fetch_add(d_off_max, 1, __ATOMIC_CONSUME);
	return off * TOYFS_PAGE_SIZE;
}

static int _toyfs_add_dentry(struct toyfs_inode_info *dir_tii,
			     struct toyfs_inode_info *tii, struct zufs_str *str)
{
	loff_t doff;
	struct toyfs_dirent *dirent;
	struct list_head *childs;
	const ino_t dirino = dir_tii->ino;
	const ino_t ino = tii->ino;

	toyfs_info("add_dentry: dirino=%lu %.*s ino=%lu mode=%o",
		   dirino, str->len, str->name, ino, toyfs_mode_of(tii));

	childs = &dir_tii->ti->ti.dir.d_childs;
	dirent = toyfs_alloc_dirent(dir_tii->sbi);
	if (!dirent)
		return -ENOSPC;

	doff = toyfs_next_doff(dir_tii);
	_set_dirent(dirent, str->name, str->len, tii, doff);
	list_add_tail(&dirent->d_head, childs);
	dir_tii->ti->ti.dir.d_ndentry++;
	dir_tii->ti->zi.i_size = (size_t)(doff + TOYFS_PAGE_SIZE + 2);
	zus_std_add_dentry(dir_tii->zii.zi, tii->zii.zi);

	toyfs_info("add_dentry: dirino=%lu dirnlink=%u dirsize=%ld "\
		   "%.*s ino=%lu nlink=%d", dirino, dir_tii->zii.zi->i_nlink,
		   (long)dir_tii->ti->zi.i_size, str->len, str->name,
		   ino, (int)tii->zii.zi->i_nlink);
	if (zi_islnk(tii->zii.zi))
		toyfs_info("add_dentry: symlnk=%s", toyfs_symlink_value(tii));
	return 0;
}

static int toyfs_add_dentry(struct zus_inode_info *dir_ii,
			    struct zus_inode_info *zii, struct zufs_str *str)
{
	return _toyfs_add_dentry(Z2II(dir_ii), Z2II(zii), str);
}

static int
_hasname(const struct toyfs_dirent *dirent, const struct zufs_str *str)
{
	return (dirent->d_nlen == str->len) &&
	       !strncmp(dirent->d_name, str->name, dirent->d_nlen);
}

static int _toyfs_remove_dentry(struct toyfs_inode_info *dir_tii,
				struct zufs_str *str)
{
	ino_t ino;
	struct toyfs_dirent *dirent = NULL;
	struct list_head *childs, *itr;
	struct toyfs_inode_info *tii;
	struct zus_inode *zi;

	toyfs_info("remove_dentry: dirino=%lu %.*s",
		   dir_tii->ino, str->len, str->name);

	childs = &dir_tii->ti->ti.dir.d_childs;
	itr = childs->next;
	while (itr != childs) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		if (_hasname(dirent, str))
			break;
		dirent = NULL;
		itr = itr->next;
	}
	if (!dirent)
		return -ENOENT;

	ino = dirent->d_ino;
	tii = toyfs_find_inode(dir_tii->sbi, ino);
	if (!tii)
		return -ENOENT;

	zi = tii->zii.zi;
	if (zi_isdir(zi) && tii->ti->ti.dir.d_ndentry)
		return -ENOTEMPTY;

	if (zi_islnk(zi))
		toyfs_info("remove_dentry(symlnk): ino=%lu symlnk=%s",
			   ino, toyfs_symlink_value(tii));
	else
		toyfs_info("remove_dentry: ino=%lu", ino);

	list_del(&dirent->d_head);
	dir_tii->ti->ti.dir.d_ndentry--;
	zus_std_remove_dentry(dir_tii->zii.zi, zi);

	/*
	 * XXX: Force free_inode by setting i_nlink to 0
	 * TODO: Maybe in zus? Maybe in zuf?
	 */
	if (zi_isdir(zi) && (zi->i_nlink == 1) && !tii->ti->ti.dir.d_ndentry)
		zi->i_nlink = 0;

	return 0;
}


static int toyfs_remove_dentry(struct zus_inode_info *dir_ii,
			       struct zufs_str *str)
{
	return _toyfs_remove_dentry(Z2II(dir_ii), str);
}

static ulong toyfs_lookup(struct zus_inode_info *dir_ii, struct zufs_str *str)
{
	ino_t ino = TOYFS_NULL_INO;
	struct toyfs_dirent *dirent = NULL;
	struct list_head *childs, *itr;
	struct toyfs_inode_info *dir_tii = Z2II(dir_ii);

	toyfs_info("lookup: dirino=%lu %.*s",
		   dir_tii->ino, str->len, str->name);

	childs = &dir_tii->ti->ti.dir.d_childs;
	itr = childs->next;
	while (itr != childs) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		if (_hasname(dirent, str)) {
			ino = dirent->d_ino;
			break;
		}
		itr = itr->next;
	}

	if (ino != TOYFS_NULL_INO)
		toyfs_info("lookup: dirino=%lu %.*s --> %lu",
			   dir_tii->ino, str->len, str->name, ino);
	else
		toyfs_info("lookup: dirino=%lu %.*s ENOENT",
			   dir_tii->ino, str->len, str->name);

	return ino;
}


struct toyfs_dir_context;
typedef bool (*toyfs_filldir_t)(struct toyfs_dir_context *, const char *,
				size_t, loff_t, ino_t, mode_t);

struct toyfs_dir_context {
	toyfs_filldir_t actor;
	loff_t pos;
};

struct toyfs_getdents_ctx {
	struct toyfs_dir_context dir_ctx;
	struct zufs_readdir_iter rdi;
	struct toyfs_inode_info *dir_tii;
	size_t emit_count;
};

static bool toyfs_filldir(struct toyfs_dir_context *dir_ctx, const char *name,
			  size_t len, loff_t pos, ino_t ino, mode_t dt)
{
	bool status;
	struct toyfs_getdents_ctx *ctx =
		container_of(dir_ctx, struct toyfs_getdents_ctx, dir_ctx);

	status = zufs_zde_emit(&ctx->rdi, ino, (uint8_t)dt,
			       pos, name, (uint8_t)len);
	if (status)
		ctx->emit_count++;

	toyfs_info_("filldir: %.*s ino=%ld dt=%d emit_count=%d status=%d",
		    (int)len, name, ino, dt, (int)ctx->emit_count, (int)status);

	return status;
}

static void _init_getdents_ctx(struct toyfs_getdents_ctx *ctx,
			       struct toyfs_inode_info *dir_tii,
			       struct zufs_ioc_readdir *ioc_readdir,
			       void *app_ptr)
{
	zufs_readdir_iter_init(&ctx->rdi, ioc_readdir, app_ptr);
	ctx->dir_ctx.actor = toyfs_filldir;
	ctx->dir_ctx.pos = ioc_readdir->pos;
	ctx->dir_tii = dir_tii;
	ctx->emit_count = 0;
}

static bool _dir_emit(struct toyfs_dir_context *ctx, const char *name,
		      size_t namelen, ino_t ino, mode_t type)
{
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type);
}

static bool
_toyfs_dir_emit(struct toyfs_dir_context *ctx,
		const struct toyfs_dirent *dirent)
{
	ctx->pos = dirent->d_off;
	return _dir_emit(ctx, dirent->d_name, dirent->d_nlen,
			 dirent->d_ino, dirent->d_type);
}

static bool _iterate_dir(struct toyfs_inode_info *dir_tii,
			 struct toyfs_dir_context *ctx)
{
	bool ok = true;
	struct toyfs_dirent *dirent;
	struct list_head *itr, *childs;
	struct toyfs_inode *dir_ti = dir_tii->ti;

	childs = &dir_ti->ti.dir.d_childs;
	if (ctx->pos == 0) {
		ok = _dir_emit(ctx, ".", 1, dir_ti->zi.i_ino, DT_DIR);
		ctx->pos = 1;
	}
	if ((ctx->pos == 1) && ok) {
		ok = _dir_emit(ctx, "..", 2, dir_ti->i_parent_ino, DT_DIR);
		ctx->pos = 2;
	}
	itr = childs->next;
	while ((itr != childs) && ok) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		itr = itr->next;

		if (dirent->d_off >= ctx->pos) {
			ok = _toyfs_dir_emit(ctx, dirent);
			ctx->pos = dirent->d_off + 1;
		}
	}
	return (itr != childs);
}

static int toyfs_iterate_dir(struct toyfs_inode_info *dir_tii,
			     struct zufs_ioc_readdir *zir, void *buf)
{

	struct toyfs_getdents_ctx ctx;

	_init_getdents_ctx(&ctx, dir_tii, zir, buf);
	zir->more = _iterate_dir(dir_tii, &ctx.dir_ctx);
	zir->pos = ctx.dir_ctx.pos;
	toyfs_info("iterate_dir: emit_count=%lu more=%d pos=%ld",
		   ctx.emit_count, (int)zir->more, zir->pos);
	return 0;
}

static int toyfs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir)
{
	int err;
	struct toyfs_inode_info *dir_tii = Z2II(zir->dir_ii);

	toyfs_info("readdir: dirino=%lu pos=%ld len=%u",
		   dir_tii->ino, zir->pos, zir->hdr.len);
	err = toyfs_iterate_dir(dir_tii, zir, app_ptr);
	toyfs_info("readdir: dirino=%lu pos=%ld len=%u dirsize=%ld err=%d",
		   dir_tii->ino, zir->pos, zir->hdr.len,
		   (long)dir_tii->zii.zi->i_size, err);
	return err;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-namei.c */

static int toyfs_setattr(struct zus_inode_info *zii,
			 uint enable_bits, ulong truncate_size)
{
	int err = 0;
	struct zus_inode *zi = zii->zi;
	struct toyfs_inode_info *tii = Z2II(zii);

	toyfs_info("setattr: ino=%lu enable_bits=%x truncate_size=%lu",
		   tii->ino, enable_bits, truncate_size);

	/* TODO: CL-FLUSH */
	if (enable_bits & STATX_MODE)
		toyfs_info("setattr: mode=%o", zi->i_mode);
	if (enable_bits & STATX_NLINK)
		toyfs_info("setattr: nlink=%o", zi->i_nlink);
	if (enable_bits & (STATX_UID | STATX_GID))
		toyfs_info("setattr: uid=%u gid=%u", zi->i_uid, zi->i_gid);
	if (enable_bits & (STATX_ATIME | STATX_MTIME | STATX_CTIME))
		toyfs_info("setattr: atime=%lu mtime=%lu ctime=%lu",
			   (uint64_t)zi->i_atime,
			   (uint64_t)zi->i_mtime,
			   (uint64_t)zi->i_ctime);

	if (enable_bits & STATX_SIZE)
		err = toyfs_truncate(tii, truncate_size);

	return err;
}

static int toyfs_rename(struct zufs_ioc_rename *zir)
{
	int err;
	struct toyfs_inode_info *old_dir_ii = Z2II(zir->old_dir_ii);
	struct toyfs_inode_info *new_dir_ii = Z2II(zir->new_dir_ii);
	struct toyfs_inode_info *old_ii = Z2II(zir->old_zus_ii);
	struct toyfs_inode_info *new_ii = Z2II(zir->new_zus_ii);
	struct zufs_str *new_name = &zir->new_d_str;
	struct zufs_str *old_name = &zir->old_d_str;

	if (!new_ii) {
		toyfs_info("rename: add_dentry: dirino=%lu ino=%lu "
			   "new_name=%.*s", new_dir_ii->ino,
			   old_ii->ino, new_name->len, new_name->name);
		err = _toyfs_add_dentry(new_dir_ii, old_ii, new_name);
		if (err)
			goto out;
		new_dir_ii->zii.zi->i_ctime = zir->time;
	}
	if (old_name->len) {
		toyfs_info("rename: remove_dentry: dirino=%lu ino=%lu "
			   "old_name=%.*s", old_dir_ii->ino,
			   old_ii->ino, old_name->len, old_name->name);
		err = _toyfs_remove_dentry(old_dir_ii, old_name);
		if (err)
			goto out;
		old_dir_ii->zii.zi->i_ctime = zir->time;
	}

out:
	toyfs_info("rename: err=%d", err);
	return err;
}

static int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_inode *ti = tii->ti;

	toyfs_info("get_symlink: ino=%lu", tii->ino);

	if (!zi_islnk(zii->zi))
		return -EINVAL;

	if (zii->zi->i_size > sizeof(zii->zi->i_symlink))
		*symlink = ti->ti.symlnk.sl_long->dat;

	else
		*symlink = zii->zi->i_symlink;
	return 0;
}

static int toyfs_sync(struct zus_inode_info *zii,
		      struct zufs_ioc_range *ioc_range)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	toyfs_info("sync: ino=%lu offset=%lu length=%lu opflags=%u",
		   tii->ino, (size_t)ioc_range->offset,
		   (size_t)ioc_range->length, ioc_range->opflags);

	/* TODO: CL_FLUSH for relevant pages */
	return 0;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-file.c */

static struct toyfs_blkref *_new_blk(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_page *page;
	struct toyfs_blkref *blkref = NULL;
	struct toyfs_sb_info *sbi = tii->sbi;
	struct zus_inode *zi = tii->zii.zi;

	page = toyfs_alloc_page(sbi);
	if (!page)
		goto out;

	blkref = toyfs_alloc_blkref(sbi);
	if (!blkref)
		goto out;

	blkref->b_off = off;
	blkref->b_bn = toyfs_addr2bn(sbi, page);
	zi->i_blocks++;

out:
	if (page && !blkref)
		toyfs_free_page(sbi, page);
	return blkref;
}

static void _free_blk(struct toyfs_inode_info *tii, struct toyfs_blkref *blkref)
{
	struct toyfs_page *page;
	struct toyfs_sb_info *sbi = tii->sbi;
	struct zus_inode *zi = tii->zii.zi;

	toyfs_assert(zi->i_blocks);

	page = toyfs_bn2page(sbi, blkref->b_bn);
	blkref->b_off = -1;
	blkref->b_bn = 0;
	toyfs_free_blkref(sbi, blkref);
	toyfs_free_page(sbi, page);
	zi->i_blocks--;
}

static void *_advance(void *buf, size_t len)
{
	return ((char *)buf + len);
}

static loff_t _off_to_boff(loff_t off)
{
	const loff_t page_size = (loff_t)TOYFS_PAGE_SIZE;
	return (off / page_size) * page_size;
}

static loff_t _off_in_page(loff_t off)
{
	const loff_t page_size = (loff_t)TOYFS_PAGE_SIZE;
	return off % page_size;
}

static loff_t _next_page(loff_t off)
{
	const loff_t page_size = TOYFS_PAGE_SIZE;

	return ((off + page_size) / page_size) * page_size;
}

static size_t _nbytes_in_range(loff_t off, loff_t next, loff_t end)
{
	return (size_t)((next < end) ? (next - off) : (end - off));
}

static void
_copy_out(void *tgt, const struct toyfs_page *page, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	memcpy(tgt, &page->dat[off], len);
}

static void
_copy_in(struct toyfs_page *page, const void *src, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	memcpy(&page->dat[off], src, len);
}

static void _fill_zeros(void *tgt, size_t len)
{
	memset(tgt, 0, len);
}

static void _assign_zeros(struct toyfs_page *page, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	_fill_zeros(&page->dat[off], len);
}

static int _check_io(loff_t off, size_t len)
{
	const size_t uoff = (size_t)off;

	if (off < 0)
		return -EINVAL;
	if (len == 0)
		return -EINVAL; /* TODO: Ignore? */
	if (uoff > TOYFS_ISIZE_MAX)
		return -EFBIG;
	if ((uoff + len) > TOYFS_ISIZE_MAX)
		return -EFBIG;
	return 0;
}

static int _check_falloc_flags(int flags)
{
	if (flags & FALLOC_FL_NO_HIDE_STALE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_COLLAPSE_RANGE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_INSERT_RANGE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_UNSHARE_RANGE)
		return -ENOTSUP;
	if ((flags & FALLOC_FL_PUNCH_HOLE) && !(flags & FALLOC_FL_KEEP_SIZE))
		return -ENOTSUP;
	return 0;
}

static loff_t _max_offset(loff_t off, size_t len, size_t isize)
{
	const loff_t end = off + (loff_t)len;

	return (end > (loff_t)isize) ? end : (loff_t)isize;
}

static loff_t _tin_offset(loff_t off, size_t len, size_t isize)
{
	const loff_t end = off + (loff_t)len;

	return (end < (loff_t)isize) ? end : (loff_t)isize;
}

static struct toyfs_blkref *
_fetch_blkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct list_head *blkrefs;
	struct toyfs_blkref *blkref;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	blkrefs = &reg_ti->r_blkrefs;
	itr = blkrefs->next;
	while (itr != blkrefs) {
		blkref = container_of(itr, struct toyfs_blkref, b_head);
		if (blkref->b_off == boff)
			return blkref;
		itr = itr->next;
	}
	return NULL;
}

static struct toyfs_page *
_fetch_page(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_blkref *blkref;
	struct toyfs_page *page = NULL;

	blkref = _fetch_blkref(tii, off);
	if (blkref)
		page = toyfs_bn2page(tii->sbi, blkref->b_bn);
	return page;
}

static void toyfs_evict(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	toyfs_info("evict: ino=%lu", tii->ino);
	/* TODO: What here? */
}

static int toyfs_read(void *buf, struct zufs_ioc_IO *ioc_io)
{
	int err;
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_page *page;
	struct toyfs_inode_info *tii = Z2II(ioc_io->zus_ii);

	off = (loff_t)ioc_io->filepos;
	len = ioc_io->hdr.len;
	toyfs_info("read: ino=%ld off=%ld len=%lu",
		   tii->ino, off, len);

	err = _check_io(off, len);
	if (err)
		return err;

	end = _tin_offset(off, len, tii->ti->zi.i_size);
	while (off < end) {
		page = _fetch_page(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		if (page)
			_copy_out(buf, page, _off_in_page(off), len);
		else
			_fill_zeros(buf, len);

		cnt += len;
		off = nxt;
		buf = _advance(buf, len);
	}

	/* TODO: Output result? */
	return 0;
}

static int toyfs_get_block(struct zus_inode_info *zii,
			   struct zufs_ioc_get_block *get_block)
{
	loff_t off;
	struct toyfs_inode_info *tii = Z2II(zii);
	const size_t blkidx = get_block->index;
	struct toyfs_page *page;

	if (!zi_isreg(tii->zii.zi))
		return -ENOTSUP;

	off = (loff_t)(blkidx * TOYFS_PAGE_SIZE);
	page = _fetch_page(tii, off);
	if (page)
		get_block->pmem_bn = toyfs_addr2bn(tii->sbi, page);
	else
		get_block->pmem_bn = 0;

	toyfs_info("get_block: ino=%ld blkidx=%lu pmem_bn=%ld",
		   tii->ino, blkidx, (long)get_block->pmem_bn);

	return 0;
}

static struct toyfs_page *
_require_page(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct toyfs_blkref *blkref = NULL;
	struct list_head *blkrefs;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	blkrefs = &reg_ti->r_blkrefs;
	itr = blkrefs->next;
	while (itr != blkrefs) {
		blkref = container_of(itr, struct toyfs_blkref, b_head);
		if (blkref->b_off == boff)
			break;
		if (blkref->b_off > boff) {
			blkref = NULL;
			break;
		}
		itr = itr->next;
		blkref = NULL;
	}
	if (!blkref) {
		blkref = _new_blk(tii, boff);
		if (blkref)
			list_add_before(&blkref->b_head, itr);
	}
	return blkref ? toyfs_bn2page(tii->sbi, blkref->b_bn) : NULL;
}

static int toyfs_write(void *buf, struct zufs_ioc_IO *ioc_io)
{
	int err;
	size_t len, cnt = 0;
	loff_t from, off, end, nxt;
	struct toyfs_page *page = NULL;
	struct toyfs_inode_info *tii = Z2II(ioc_io->zus_ii);

	off = from = (loff_t)ioc_io->filepos;
	len = ioc_io->hdr.len;
	toyfs_info("write: ino=%ld off=%ld len=%lu",
		   tii->ino, off, len);

	err = _check_io(off, len);
	if (err)
		return err;

	end = off + (loff_t)len;
	while (off < end) {
		page = _require_page(tii, off);
		if (!page)
			break;

		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_copy_in(page, buf, _off_in_page(off), len);

		cnt += len;
		off = nxt;
		buf = _advance(buf, len);
	}
	tii->zii.zi->i_size =
		(size_t)_max_offset(from, cnt, tii->zii.zi->i_size);
	return page ? 0 : -ENOSPC;
}

static void _zero_range_at(struct toyfs_inode_info *tii,
			   struct toyfs_blkref *blkref, loff_t off, size_t len)
{
	struct toyfs_page *page;

	if (blkref) {
		toyfs_info("zero range: ino=%lu off=%ld len=%lu bn=%lu",
			   tii->ino, off, len, blkref->b_bn);
		page = toyfs_bn2page(tii->sbi, blkref->b_bn);
		_assign_zeros(page, _off_in_page(off), len);
	}
}

static void _punch_hole_at(struct toyfs_inode_info *tii,
			   struct toyfs_blkref *blkref, loff_t off, size_t len)
{
	if (blkref) {
		if (len < TOYFS_PAGE_SIZE)
			_zero_range_at(tii, blkref, off, len);
		else
			_drop_blkref(tii, blkref);
	}
}

static int
toyfs_punch_hole(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_blkref *blkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		blkref = _fetch_blkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_punch_hole_at(tii, blkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int
toyfs_zero_range(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_blkref *blkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		blkref = _fetch_blkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_zero_range_at(tii, blkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int
toyfs_falloc_range(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_page *page = NULL;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		page = _require_page(tii, off);
		if (!page)
			break;

		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);

		cnt += len;
		off = nxt;
	}

	tii->zii.zi->i_size =
		(size_t)_max_offset(from, cnt, tii->zii.zi->i_size);
	return page ? 0 : -ENOSPC;
}

static int toyfs_fallocate(struct zus_inode_info *zii,
			   struct zufs_ioc_range *ioc_range)
{
	int err, flags;
	size_t len;
	loff_t off;
	struct toyfs_inode_info *tii = Z2II(zii);

	off = (loff_t)ioc_range->offset;
	len = (size_t)ioc_range->length;
	flags = (int)ioc_range->opflags;
	toyfs_info("fallocate: ino=%lu offset=%ld length=%lu flags=%d",
		   tii->ino, off, len, flags);

	err = _check_io(off, len);
	if (err)
		goto out;
	err = _check_falloc_flags(flags);
	if (err)
		goto out;

	if (flags & FALLOC_FL_PUNCH_HOLE)
		err = toyfs_punch_hole(tii, off, len);
	else if (flags & FALLOC_FL_ZERO_RANGE)
		err = toyfs_zero_range(tii, off, len);
	else
		err = toyfs_falloc_range(tii, off, len);
out:
	return err;
}

static int _seek_block(struct toyfs_inode_info *tii, loff_t from,
		       bool seek_exist, loff_t *out_off)
{
	loff_t off, end;
	const struct toyfs_page *page;

	off = from;
	end = tii->zii.zi->i_size;
	while (off < end) {
		page = _fetch_page(tii, off);
		if ((page && seek_exist) || (!page && !seek_exist)) {
			*out_off = off;
			break;
		}
		off = _next_page(off);
	}
	return 0;
}

static int
toyfs_seek_data(struct toyfs_inode_info *tii, loff_t from, loff_t *out_off)
{
	return _seek_block(tii, from, true, out_off);
}

static int
toyfs_seek_hole(struct toyfs_inode_info *tii, loff_t from, loff_t *out_off)
{
	return _seek_block(tii, from, false, out_off);
}

static int toyfs_seek(struct zus_inode_info *zii,
		      struct zufs_ioc_seek *ioc_seek)
{
	int err, whence = (int)ioc_seek->whence;
	loff_t off_in, off = -1;
	struct toyfs_inode_info *tii = Z2II(zii);

	off_in = (loff_t)ioc_seek->offset_in;
	toyfs_info("seek: ino=%lu offset_in=%ld whence=%d",
		   tii->ino, off_in, whence);

	if (whence == SEEK_DATA)
		err = toyfs_seek_data(tii, off_in, &off);
	else if (whence == SEEK_HOLE)
		err = toyfs_seek_hole(tii, off_in, &off);
	else
		err = -ENOTSUP;

	ioc_seek->offset_out = (uint64_t)off;
	return err;
}

static void _drop_blkref(struct toyfs_inode_info *tii,
			 struct toyfs_blkref *blkref)
{
	if (blkref) {
		toyfs_info("drop page: ino=%lu off=%ld bn=%lu",
			   tii->ino, blkref->b_off, blkref->b_bn);
		list_del(&blkref->b_head);
		_free_blk(tii, blkref);
	}
}

static void _drop_range(struct toyfs_inode_info *tii, loff_t pos)
{
	struct list_head *itr;
	struct toyfs_blkref *blkref = NULL;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	struct list_head *blkrefs = &reg_ti->r_blkrefs;

	if (pos % TOYFS_PAGE_SIZE)
		pos = _next_page(pos);

	itr = blkrefs->next;
	while (itr != blkrefs) {
		blkref = container_of(itr, struct toyfs_blkref, b_head);
		itr = itr->next;

		if (blkref->b_off >= pos)
			_drop_blkref(tii, blkref);
	}
}

static int toyfs_truncate(struct toyfs_inode_info *tii, size_t size)
{
	struct zus_inode *zi = tii->zii.zi;

	if (S_ISDIR(zi->i_mode))
		return -EISDIR;

	if (!S_ISREG(zi->i_mode))
		return -EINVAL;

	if (size < zi->i_size)
		_drop_range(tii, (loff_t)size);

	zi->i_size = size;
	return 0;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/

static const struct zus_zii_operations toyfs_zii_op = {
	.evict = toyfs_evict,
	.read = toyfs_read,
	.write = toyfs_write,
	.setattr = toyfs_setattr,
	.get_symlink = toyfs_get_symlink,
	.sync = toyfs_sync,
	.fallocate = toyfs_fallocate,
	.seek = toyfs_seek,
	.get_block = toyfs_get_block,
};

static const struct zus_sbi_operations toyfs_sbi_op = {
	.zii_alloc = toyfs_zii_alloc,
	.zii_free = toyfs_zii_free,
	.new_inode = toyfs_new_inode,
	.free_inode = toyfs_free_inode,
	.add_dentry = toyfs_add_dentry,
	.remove_dentry = toyfs_remove_dentry,
	.lookup = toyfs_lookup,
	.iget = toyfs_iget,
	.rename = toyfs_rename,
	.readdir = toyfs_readdir,
	.statfs = toyfs_statfs,

};

static const struct zus_zfi_operations toyfs_zfi_op = {
	.sbi_alloc = toyfs_sbi_alloc,
	.sbi_free = toyfs_sbi_free,
	.sbi_init = toyfs_sbi_init,
	.sbi_fini = toyfs_sbi_fini,
};

/* Is not const because it is hanged on a list_head */
static struct zus_fs_info toyfs_zfi = {
	.rfi.fsname = "toyfs",
	.rfi.FS_magic = ZUFS_SUPER_MAGIC,
	.rfi.ver_minor = 14,
	.rfi.ver_major = 0,
	.rfi.dt_offset = 0,
	.rfi.s_time_gran = 1,
	.rfi.def_mode = 0755,
	.rfi.s_maxbytes = MAX_LFS_FILESIZE,
	.rfi.acl_on = 1,
	.op = &toyfs_zfi_op,
	.sbi_op = &toyfs_sbi_op,
	.user_page_size = 0,
	.next_sb_id = 0,
};

#define TOYFS_STATICASSERT_SIZEOFPAGE(t) \
	TOYFS_STATICASSERT_EQ(sizeof(t), TOYFS_PAGE_SIZE)

static void toyfs_check_types(void)
{
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_pool_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_inodes_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_blkrefs_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_dirents_page);
}

int toyfs_register_fs(int fd)
{
	toyfs_check_types();
	return zus_register_one(fd, &toyfs_zfi);
}
