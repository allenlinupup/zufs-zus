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

/* CentOS7 workarounds */
#ifndef FALLOC_FL_INSERT_RANGE
#define FALLOC_FL_INSERT_RANGE	0x20
#endif

#ifndef FALLOC_FL_UNSHARE_RANGE
#define FALLOC_FL_UNSHARE_RANGE	0x40
#endif


#define TOYFS_STATICASSERT(expr)	_Static_assert(expr, #expr)
#define TOYFS_STATICASSERT_EQ(a, b)	TOYFS_STATICASSERT(a == b)
#define TOYFS_STATICASSERT_SIZEOFPAGE(t) \
	TOYFS_STATICASSERT_EQ(sizeof(t), PAGE_SIZE)
#define TOYFS_ISIZE_MAX		(1ULL << 50)
#define TOYFS_IMAGIC		(0x11E11F5)


#define toyfs_panic(fmt, ...) \
	toyfs_panicf(__FILE__, __LINE__, fmt, __VA_ARGS__)
#define toyfs_panic_if_err(err, msg) \
	do { if (err) toyfs_panic("%s: %d", msg, err); } while (0)
#define toyfs_assert(cond) \
	do { if (!(cond)) toyfs_panic("assert failed: %s", #cond); } while (0)


#define Z2SBI(zsbi) _zsbi_to_sbi(zsbi)
#define Z2II(zii) _zii_to_tii(zii)


static const struct zus_zii_operations toyfs_zii_op;
static const struct zus_zfi_operations toyfs_zfi_op;
static const struct zus_sbi_operations toyfs_sbi_op;
static int toyfs_truncate(struct toyfs_inode_info *tii, size_t size);
static struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi);
static size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr);
static void _drop_iblkref(struct toyfs_inode_info *, struct toyfs_iblkref *);


/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-common.[ch] */

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

#define TOYFS_DBLKREFS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_dblkref))

#define TOYFS_IBLKREFS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_iblkref))

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

union toyfs_dblkrefs_page {
	struct toyfs_page page;
	struct toyfs_dblkref dblkrefs[TOYFS_DBLKREFS_PER_PAGE];
};

union toyfs_iblkrefs_page {
	struct toyfs_page page;
	struct toyfs_iblkref iblkrefs[TOYFS_IBLKREFS_PER_PAGE];
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
		INFO("mmap ok: %p\n", mem);
	} else {
		err = -errno;
		ERROR("mmap failed: %d\n", err);
	}
	return err;
}

static void _munmap_memory(void *mem, size_t msz)
{
	if (mem) {
		INFO("munmap %p %lu\n", mem, msz);
		munmap(mem, msz);
	}
}

static void _pool_init(struct toyfs_pool *pool)
{
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	list_init(&pool->free_dblkrefs);
	list_init(&pool->free_iblkrefs);
	list_init(&pool->free_dirents);
	list_init(&pool->free_inodes);
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

static int _pool_add_free_inodes(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_inodes_page *ipage;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	ipage = (union toyfs_inodes_page *)page;
	for (i = 0; i < ARRAY_SIZE(ipage->inodes); ++i)
		list_add(&ipage->inodes[i].head, &pool->free_inodes);

	return 0;
}

static struct toyfs_inode *_list_head_to_inode(struct list_head *head)
{
	union toyfs_inode_head *ihead;

	ihead = container_of(head, union toyfs_inode_head, head);
	return &ihead->inode;
}

static struct toyfs_inode *_pool_pop_free_inode(struct toyfs_pool *pool)
{
	struct toyfs_inode *ti = NULL;

	if (!list_empty(&pool->free_inodes)) {
		ti = _list_head_to_inode(pool->free_inodes.next);
		list_del(pool->free_inodes.next);
	}
	return ti;
}

static struct toyfs_inode *_pool_pop_inode(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_inode *ti;

	_pool_lock(pool);
	ti = _pool_pop_free_inode(pool);
	if (ti)
		goto out;

	err = _pool_add_free_inodes(pool);
	if (err)
		goto out;

	ti = _pool_pop_free_inode(pool);
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
	list_add_tail(&ihead->head, &pool->free_inodes);
	_pool_unlock(pool);
}

static int _pool_add_free_dirents(struct toyfs_pool *pool)
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
		list_add_tail(&dirent->d_head, &pool->free_dirents);
	}
	return 0;
}

static struct toyfs_dirent *_pool_pop_free_dirent(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_dirent *dirent = NULL;

	if (!list_empty(&pool->free_dirents)) {
		elem = pool->free_dirents.next;
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
	dirent = _pool_pop_free_dirent(pool);
	if (!dirent) {
		err = _pool_add_free_dirents(pool);
		if (!err)
			dirent = _pool_pop_free_dirent(pool);
	}
	_pool_unlock(pool);
	return dirent;
}

static void _pool_push_dirent(struct toyfs_pool *pool,
			      struct toyfs_dirent *dirent)
{
	_pool_lock(pool);
	list_add_tail(&dirent->d_head, &pool->free_dirents);
	_pool_unlock(pool);
}

static int _pool_add_free_dblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_dblkrefs_page *ppage;
	struct toyfs_dblkref *dblkref;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	ppage = (union toyfs_dblkrefs_page *)page;
	for (i = 0; i < ARRAY_SIZE(ppage->dblkrefs); ++i) {
		dblkref = &ppage->dblkrefs[i];
		list_add_tail(&dblkref->head, &pool->free_dblkrefs);
	}
	return 0;
}

static struct toyfs_dblkref *_pool_pop_free_dblkref(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_dblkref *dblkref = NULL;

	if (!list_empty(&pool->free_dblkrefs)) {
		elem = pool->free_dblkrefs.next;
		list_del(elem);
		dblkref = container_of(elem, struct toyfs_dblkref, head);
	}
	return dblkref;
}

static struct toyfs_dblkref *_pool_pop_dblkref(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_dblkref *dblkref;

	_pool_lock(pool);
	dblkref = _pool_pop_free_dblkref(pool);
	if (!dblkref) {
		err = _pool_add_free_dblkrefs(pool);
		if (!err)
			dblkref = _pool_pop_free_dblkref(pool);
	}
	_pool_unlock(pool);
	return dblkref;
}

static void _pool_push_dblkref(struct toyfs_pool *pool,
				      struct toyfs_dblkref *dblkref)
{
	_pool_lock(pool);
	list_add(&dblkref->head, &pool->free_dblkrefs);
	_pool_unlock(pool);
}

static int _pool_add_free_iblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_iblkrefs_page *bpage;
	struct toyfs_iblkref *iblkref;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	bpage = (union toyfs_iblkrefs_page *)page;
	for (i = 0; i < ARRAY_SIZE(bpage->iblkrefs); ++i) {
		iblkref = &bpage->iblkrefs[i];
		list_add_tail(&iblkref->head, &pool->free_iblkrefs);
	}
	return 0;
}

static struct toyfs_iblkref *_pool_pop_free_iblkref(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_iblkref *iblkref = NULL;

	if (!list_empty(&pool->free_iblkrefs)) {
		elem = pool->free_iblkrefs.next;
		list_del(elem);
		iblkref = container_of(elem, struct toyfs_iblkref, head);
	}
	return iblkref;
}

static struct toyfs_iblkref *_pool_pop_iblkref(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_iblkref *iblkref;

	_pool_lock(pool);
	iblkref = _pool_pop_free_iblkref(pool);
	if (!iblkref) {
		err = _pool_add_free_iblkrefs(pool);
		if (!err)
			iblkref = _pool_pop_free_iblkref(pool);
	}
	_pool_unlock(pool);
	return iblkref;
}

static void _pool_push_iblkref(struct toyfs_pool *pool,
			       struct toyfs_iblkref *iblkref)
{
	_pool_lock(pool);
	list_add(&iblkref->head, &pool->free_iblkrefs);
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

	INFO("sbi_alloc: zfi=%p\n", zfi);

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

	INFO("sbi_free: zsbi=%p\n", zsbi);
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
	DBG("alloc_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
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
	DBG("free_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
	    sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
	    toyfs_addr2bn(sbi, page));
	_sbi_unlock(sbi);
}

static struct toyfs_dblkref *_consume_dblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_dblkref *dblkref;

	dblkref = _pool_pop_dblkref(&sbi->s_pool);
	if (dblkref) {
		dblkref->refcnt = 0;
		dblkref->bn = 0;
	}
	return dblkref;
}

static void _release_dblkref(struct toyfs_sb_info *sbi,
			     struct toyfs_dblkref *dblkref)
{
	dblkref->bn = 0;
	_pool_push_dblkref(&sbi->s_pool, dblkref);
}

static struct toyfs_iblkref *_consume_iblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_iblkref *iblkref;

	iblkref = _pool_pop_iblkref(&sbi->s_pool);
	if (iblkref) {
		iblkref->off = -1;
		iblkref->dblkref = NULL;
	}
	return iblkref;
}

static void _release_iblkref(struct toyfs_sb_info *sbi,
			     struct toyfs_iblkref *iblkref)
{
	iblkref->dblkref = NULL;
	iblkref->off = -1;
	_pool_push_iblkref(&sbi->s_pool, iblkref);
}

static struct toyfs_dirent *toyfs_alloc_dirent(struct toyfs_sb_info *sbi)
{
	return _pool_pop_dirent(&sbi->s_pool);
}

static void toyfs_free_dirent(struct toyfs_sb_info *sbi,
			      struct toyfs_dirent *dirent)
{
	return _pool_push_dirent(&sbi->s_pool, dirent);
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
	sbi->s_statvfs.f_namemax = ZUFS_NAME_LEN;
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
	root_i->ti_zi.i_blksize = PAGE_SIZE;
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
		ERROR("illegal magic1: %ld\n",
		      (long)sb->part1.dev_table.s_magic);
		return -EINVAL;
	}
	if (sb->part2.dev_table.s_magic != ZUFS_SUPER_MAGIC) {
		ERROR("illegal magic2: %ld\n",
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
	char buf[1024];
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
	char buf[1024];
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

	INFO("sbi_init: zsbi=%p\n", zsbi);

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
	zim->s_blocksize_bits  = PAGE_SHIFT;

	return 0;
}

static int toyfs_sbi_fini(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_fini: zsbi=%p\n", zsbi);

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
	DBG("find_inode: ino=%lu\n", ino);
	return _itable_find(&sbi->s_itable, ino);
}

static int
toyfs_iget(struct zus_sb_info *zsbi, struct zus_inode_info *zii, ulong ino)
{
	int err = 0;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii;

	DBG("iget: ino=%lu\n", ino);

	toyfs_assert(zii->op);
	tii = toyfs_find_inode(sbi, ino);
	if (tii) {
		zii->zi = tii->zii.zi;
		DBG("iget: ino=%lu zi=%p\n", ino, zii->zi);
	} else {
		err = -ENOENT;
		DBG("iget: ino=%lu err=%d\n", ino, err);
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

	DBG("alloc_ii tii=%p files=%lu ffree=%lu\n", tii,
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
	DBG("free_ii tii=%p files=%lu ffree=%lu\n", tii,
	    sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);
}

static struct zus_inode_info *toyfs_zii_alloc(struct zus_sb_info *zsbi)
{
	struct toyfs_inode_info *tii;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	_sbi_lock(sbi);
	tii = toyfs_alloc_ii(sbi);
	_sbi_unlock(sbi);

	DBG("zii_alloc: zii=%p\n", &tii->zii);

	return tii ? &tii->zii : NULL;
}

static void toyfs_zii_free(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;

	DBG("zii_free: zii=%p\n", zii);

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

	DBG("statfs sbi=%p\n", sbi);

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

	DBG("statfs: bsize=%ld blocks=%ld bfree=%ld bavail=%ld "
	    "files=%ld ffree=%ld\n", (long)out->f_bsize,
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
	DBG("new_inode: zsbi=%p zii=%p mode=%o\n", zsbi, zii, mode);

	if (!(zi_isdir(zi) || zi_isreg(zi) || zi_islnk(zi) || S_ISFIFO(mode)))
		return -ENOTSUP;
	if (zi->i_size >= PAGE_SIZE)
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
		DBG("new_inode(dir): ino=%lu\n", ino);
		list_init(&ti->ti.dir.d_childs);
		ti->ti.dir.d_ndentry = 0;
		ti->ti.dir.d_off_max = 2;
		ti->zi.i_size = PAGE_SIZE;
		ti->i_parent_ino = dir_tii->zii.zi->i_ino;
		zus_std_new_dir(dir_tii->zii.zi, &ti->zi);
	} else if (zi_isreg(zi)) {
		DBG("new_inode(reg): ino=%lu\n", ino);
		list_init(&ti->ti.reg.r_iblkrefs);
		ti->ti.reg.r_first_parent = dir_tii->zii.zi->i_ino;
		if (ioc_new->flags & ZI_TMPFILE)
			ti->zi.i_nlink = 1;
	} else if (zi_islnk(zi)) {
		symlen = ti->zi.i_size;
		symlong = symlen > sizeof(ti->zi.i_symlink);
		symname = symlong ? (const char *)app_ptr :
			  (const char *)zi->i_symlink;
		DBG("new_inode(symlnk): ino=%lu lnk=%.*s\n",
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
		DBG("new_inode(fifo): ino=%lu\n", ino);
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

	DBG("free_inode: ino=%lu mode=%o nlink=%ld size=%ld\n",
	    tii->ino, (int)zi->i_mode,
	    (long)zi->i_nlink, (long)zi->i_size);

	if (zi_isdir(zi)) {
		DBG("free_inode(dir): ino=%lu\n", tii->ino);
		if (tii->ti->ti.dir.d_ndentry)
			return -ENOTEMPTY;
		zi->i_dir.parent = 0; /* TODO: Maybe zus_std helper ? */
	} else if (zi_islnk(zi)) {
		DBG("free_inode(symlink): ino=%lu symlnk=%s\n",
		    tii->ino, toyfs_symlink_value(tii));
		toyfs_release_symlink(tii);
	} else if (zi_isreg(zi)) {
		DBG("free_inode(reg): ino=%lu\n", tii->ino);
		toyfs_truncate(tii, 0);
	} else {
		DBG("free_inode: ino=%lu mode=%o\n", tii->ino, zi->i_mode);
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
	return off * PAGE_SIZE;
}

static int _toyfs_add_dentry(struct toyfs_inode_info *dir_tii,
			     struct toyfs_inode_info *tii, struct zufs_str *str)
{
	loff_t doff;
	struct toyfs_dirent *dirent;
	struct list_head *childs;
	const ino_t dirino = dir_tii->ino;
	const ino_t ino = tii->ino;

	DBG("add_dentry: dirino=%lu %.*s ino=%lu mode=%o\n",
	    dirino, str->len, str->name, ino, toyfs_mode_of(tii));

	childs = &dir_tii->ti->ti.dir.d_childs;
	dirent = toyfs_alloc_dirent(dir_tii->sbi);
	if (!dirent)
		return -ENOSPC;

	doff = toyfs_next_doff(dir_tii);
	_set_dirent(dirent, str->name, str->len, tii, doff);
	list_add_tail(&dirent->d_head, childs);
	dir_tii->ti->ti.dir.d_ndentry++;
	dir_tii->ti->zi.i_size = (size_t)(doff + PAGE_SIZE + 2);
	zus_std_add_dentry(dir_tii->zii.zi, tii->zii.zi);

	DBG("add_dentry: dirino=%lu dirnlink=%u dirsize=%ld "\
	    "%.*s ino=%lu nlink=%d\n", dirino, dir_tii->zii.zi->i_nlink,
	    (long)dir_tii->ti->zi.i_size, str->len, str->name,
	    ino, (int)tii->zii.zi->i_nlink);
	if (zi_islnk(tii->zii.zi))
		DBG("add_dentry: symlnk=%s\n", toyfs_symlink_value(tii));
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
	mode_t mode;
	struct toyfs_dirent *dirent = NULL;
	struct list_head *childs, *itr;
	struct toyfs_inode_info *tii;
	struct zus_inode *zi;
	const char *symval;

	DBG("remove_dentry: dirino=%lu %.*s\n",
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

	if (zi_islnk(zi)) {
		symval = toyfs_symlink_value(tii);
		DBG("remove_dentry(symlnk): ino=%lu symlnk=%s\n", ino, symval);
	} else {
		mode = zi->i_mode;
		DBG("remove_dentry: ino=%lu mode=%o\n", ino, mode);
	}

	list_del(&dirent->d_head);
	dir_tii->ti->ti.dir.d_ndentry--;
	zus_std_remove_dentry(dir_tii->zii.zi, zi);
	toyfs_free_dirent(dir_tii->sbi, dirent);

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

	DBG("lookup: dirino=%lu %.*s\n",
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
	/*
	if (ino != TOYFS_NULL_INO)
		DBG("lookup: dirino=%lu %.*s --> %lu\n",
		     dir_tii->ino, str->len, str->name, ino);
	else
		DBG("lookup: dirino=%lu %.*s ENOENT\n",
		     dir_tii->ino, str->len, str->name);
	*/

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
	DBG("filldir: %.*s ino=%ld dt=%d emit_count=%d status=%d\n",
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
	DBG("iterate_dir: emit_count=%lu more=%d pos=%ld\n",
	    ctx.emit_count, (int)zir->more, zir->pos);
	return 0;
}

static int toyfs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir)
{
	int err;
	struct toyfs_inode_info *dir_tii = Z2II(zir->dir_ii);

	DBG("readdir: dirino=%lu pos=%ld len=%u\n",
	    dir_tii->ino, zir->pos, zir->hdr.len);
	err = toyfs_iterate_dir(dir_tii, zir, app_ptr);
	DBG("readdir: dirino=%lu pos=%ld len=%u dirsize=%ld err=%d\n",
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

	DBG("setattr: ino=%lu enable_bits=%x truncate_size=%lu\n",
	    tii->ino, enable_bits, truncate_size);

	/* TODO: CL-FLUSH */
	if (enable_bits & STATX_MODE)
		DBG("setattr: mode=%o\n", zi->i_mode);
	if (enable_bits & STATX_NLINK)
		DBG("setattr: nlink=%o\n", zi->i_nlink);
	if (enable_bits & (STATX_UID | STATX_GID))
		DBG("setattr: uid=%u gid=%u\n", zi->i_uid, zi->i_gid);
	if (enable_bits & (STATX_ATIME | STATX_MTIME | STATX_CTIME))
		DBG("setattr: atime=%lu mtime=%lu ctime=%lu\n",
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
		DBG("rename: add_dentry: dirino=%lu ino=%lu "
		    "new_name=%.*s\n", new_dir_ii->ino,
		    old_ii->ino, new_name->len, new_name->name);
		err = _toyfs_add_dentry(new_dir_ii, old_ii, new_name);
		if (err)
			goto out;
		new_dir_ii->zii.zi->i_ctime = zir->time;
	}
	if (old_name->len) {
		DBG("rename: remove_dentry: dirino=%lu ino=%lu "
		    "old_name=%.*s\n", old_dir_ii->ino,
		    old_ii->ino, old_name->len, old_name->name);
		err = _toyfs_remove_dentry(old_dir_ii, old_name);
		if (err)
			goto out;
		old_dir_ii->zii.zi->i_ctime = zir->time;
	}

out:
	DBG("rename: err=%d\n", err);
	return err;
}

static int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_inode *ti = tii->ti;

	DBG("get_symlink: ino=%lu\n", tii->ino);

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

	DBG("sync: ino=%lu offset=%lu length=%lu opflags=%u\n",
	    tii->ino, (size_t)ioc_range->offset,
	    (size_t)ioc_range->length, ioc_range->opflags);

	/* TODO: CL_FLUSH for relevant pages */
	return 0;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/
/* toyfs-file.c */

static struct toyfs_dblkref *_new_dblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_page *page;
	struct toyfs_dblkref *dblkref = NULL;

	page = toyfs_alloc_page(sbi);
	if (!page)
		goto out;

	dblkref = _consume_dblkref(sbi);
	if (!dblkref)
		goto out;

	dblkref->bn = toyfs_addr2bn(sbi, page);
	dblkref->refcnt = 1;

out:
	if (page && !dblkref)
		toyfs_free_page(sbi, page);
	return dblkref;
}

static void _free_dblkref(struct toyfs_sb_info *sbi,
			  struct toyfs_dblkref *dblkref)
{
	const size_t bn = dblkref->bn;

	_release_dblkref(sbi, dblkref);
	toyfs_free_page(sbi, toyfs_bn2page(sbi, bn));
}

static void _decref_dblkref(struct toyfs_sb_info *sbi,
			    struct toyfs_dblkref *dblkref)
{
	size_t refcnt;

	_sbi_lock(sbi);
	toyfs_assert(dblkref->refcnt > 0);
	dblkref->refcnt--;
	refcnt = dblkref->refcnt;
	_sbi_unlock(sbi);

	if (!refcnt)
		_free_dblkref(sbi, dblkref);
}

static struct toyfs_iblkref *
_new_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_dblkref *dblkref = NULL;
	struct toyfs_iblkref *iblkref = NULL;
	struct zus_inode *zi = tii->zii.zi;

	dblkref = _new_dblkref(tii->sbi);
	if (!dblkref)
		goto out;

	iblkref = _consume_iblkref(tii->sbi);
	if (!iblkref)
		goto out;

	iblkref->dblkref = dblkref;
	iblkref->off = off;
	zi->i_blocks++;

out:
	if (!iblkref && dblkref)
		_decref_dblkref(tii->sbi, dblkref);
	return iblkref;
}

static void
_free_iblkref(struct toyfs_inode_info *tii, struct toyfs_iblkref *iblkref)
{
	struct zus_inode *zi = tii->zii.zi;

	toyfs_assert(zi->i_blocks);

	_decref_dblkref(tii->sbi, iblkref->dblkref);
	_release_iblkref(tii->sbi, iblkref);
	zi->i_blocks--;
}

static void *_advance(void *buf, size_t len)
{
	return ((char *)buf + len);
}

static loff_t _off_to_boff(loff_t off)
{
	const loff_t page_size = (loff_t)PAGE_SIZE;
	return (off / page_size) * page_size;
}

static loff_t _off_in_page(loff_t off)
{
	const loff_t page_size = (loff_t)PAGE_SIZE;
	return off % page_size;
}

static loff_t _next_page(loff_t off)
{
	const loff_t page_size = PAGE_SIZE;

	return ((off + page_size) / page_size) * page_size;
}

static bool _ispagealigned(loff_t off, size_t len)
{
	const loff_t noff = off + (loff_t)len;

	return (noff == _off_to_boff(noff));
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

static void _copy_page(struct toyfs_page *page, const struct toyfs_page *other)
{
	_copy_in(page, other->dat, 0, sizeof(other->dat));
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

static int _check_rw(loff_t off, size_t len)
{
	if (len > ZUS_API_MAP_MAX_SIZE) {
		ERROR("illegal: off=%ld len=%lu\n", off, len);
		return -EINVAL;
	}
	return _check_io(off, len);
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

static struct toyfs_iblkref *
_fetch_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct list_head *iblkrefs;
	struct toyfs_iblkref *iblkref;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	iblkrefs = &reg_ti->r_iblkrefs;
	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		if (iblkref->off == boff)
			return iblkref;
		itr = itr->next;
	}
	return NULL;
}

static struct toyfs_page *
_fetch_page(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_iblkref *iblkref;
	struct toyfs_page *page = NULL;

	iblkref = _fetch_iblkref(tii, off);
	if (iblkref)
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);
	return page;
}

static void toyfs_evict(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("evict: ino=%lu\n", tii->ino);
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
	DBG("read: ino=%ld off=%ld len=%lu\n", tii->ino, off, len);

	err = _check_rw(off, len);
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

	off = (loff_t)(blkidx * PAGE_SIZE);
	page = _fetch_page(tii, off);
	if (page)
		get_block->pmem_bn = toyfs_addr2bn(tii->sbi, page);
	else
		get_block->pmem_bn = 0;

	DBG("get_block: ino=%ld blkidx=%lu pmem_bn=%ld\n",
	    tii->ino, blkidx, (long)get_block->pmem_bn);

	return 0;
}

static void _clone_data(struct toyfs_sb_info *sbi,
			struct toyfs_dblkref *dst_dblkref,
			const struct toyfs_dblkref *src_dblkref)
{
	struct toyfs_page *dst_page;
	const struct toyfs_page *src_page;

	_sbi_lock(sbi);
	dst_page = toyfs_bn2page(sbi, dst_dblkref->bn);
	src_page = toyfs_bn2page(sbi, src_dblkref->bn);
	_copy_page(dst_page, src_page);
	_sbi_unlock(sbi);
}

static struct toyfs_iblkref *
_require_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct toyfs_dblkref *dblkref;
	struct toyfs_iblkref *iblkref = NULL;
	struct list_head *iblkrefs;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	iblkrefs = &reg_ti->r_iblkrefs;
	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		if (iblkref->off == boff)
			break;
		if (iblkref->off > boff) {
			iblkref = NULL;
			break;
		}
		itr = itr->next;
		iblkref = NULL;
	}
	if (!iblkref) {
		iblkref = _new_iblkref(tii, boff);
		if (!iblkref)
			return NULL;
		list_add_before(&iblkref->head, itr);
	} else if (iblkref->dblkref->refcnt > 1) {
		dblkref = _new_dblkref(tii->sbi);
		if (!dblkref)
			return NULL;
		_clone_data(tii->sbi, dblkref, iblkref->dblkref);
		iblkref->dblkref = dblkref;
	}
	return iblkref;
}

static int toyfs_write(void *buf, struct zufs_ioc_IO *ioc_io)
{
	int err;
	size_t len, cnt = 0;
	loff_t from, off, end, nxt;
	struct toyfs_iblkref *iblkref;
	struct toyfs_page *page = NULL;
	struct toyfs_inode_info *tii = Z2II(ioc_io->zus_ii);

	off = from = (loff_t)ioc_io->filepos;
	len = ioc_io->hdr.len;
	DBG("write: ino=%ld off=%ld len=%lu\n", tii->ino, off, len);

	err = _check_rw(off, len);
	if (err)
		return err;

	end = off + (loff_t)len;
	while (off < end) {
		iblkref = _require_iblkref(tii, off);
		if (!iblkref)
			return -ENOSPC;
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);

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
			   struct toyfs_iblkref *iblkref, loff_t off, size_t len)
{
	struct toyfs_page *page;

	if (iblkref) {
		DBG("zero range: ino=%lu off=%ld len=%lu bn=%lu\n",
		    tii->ino, off, len, iblkref->dblkref->bn);
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);
		_assign_zeros(page, _off_in_page(off), len);
	}
}

static void _punch_hole_at(struct toyfs_inode_info *tii,
			   struct toyfs_iblkref *iblkref, loff_t off, size_t len)
{
	if (iblkref) {
		if (len < PAGE_SIZE)
			_zero_range_at(tii, iblkref, off, len);
		else
			_drop_iblkref(tii, iblkref);
	}
}

static int
toyfs_punch_hole(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _fetch_iblkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_punch_hole_at(tii, iblkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int
_zero_range(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _fetch_iblkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_zero_range_at(tii, iblkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int
_falloc_range(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref = NULL;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _require_iblkref(tii, off);
		if (!iblkref)
			return -ENOSPC;

		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);

		cnt += len;
		off = nxt;
	}

	tii->zii.zi->i_size =
		(size_t)_max_offset(from, cnt, tii->zii.zi->i_size);
	return 0;
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
	DBG("fallocate: ino=%lu offset=%ld length=%lu flags=%d\n",
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
		err = _zero_range(tii, off, len);
	else
		err = _falloc_range(tii, off, len);
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
_seek_data(struct toyfs_inode_info *tii, loff_t from, loff_t *out_off)
{
	return _seek_block(tii, from, true, out_off);
}

static int
_seek_hole(struct toyfs_inode_info *tii, loff_t from, loff_t *out_off)
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
	DBG("seek: ino=%lu offset_in=%ld whence=%d\n",
	    tii->ino, off_in, whence);

	if (whence == SEEK_DATA)
		err = _seek_data(tii, off_in, &off);
	else if (whence == SEEK_HOLE)
		err = _seek_hole(tii, off_in, &off);
	else
		err = -ENOTSUP;

	ioc_seek->offset_out = (uint64_t)off;
	return err;
}

static void _drop_iblkref(struct toyfs_inode_info *tii,
			  struct toyfs_iblkref *iblkref)
{
	if (iblkref) {
		DBG("drop page: ino=%lu off=%ld bn=%lu\n",
		    tii->ino, iblkref->off, iblkref->dblkref->bn);
		list_del(&iblkref->head);
		_free_iblkref(tii, iblkref);
	}
}

static void _drop_range(struct toyfs_inode_info *tii, loff_t pos)
{
	struct list_head *itr;
	struct toyfs_iblkref *iblkref = NULL;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	struct list_head *iblkrefs = &reg_ti->r_iblkrefs;

	if (pos % PAGE_SIZE)
		pos = _next_page(pos);

	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		itr = itr->next;

		if (iblkref->off >= pos)
			_drop_iblkref(tii, iblkref);
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

static int _clone_entire_file_range(struct toyfs_inode_info *src_tii,
				    struct toyfs_inode_info *dst_tii)
{
	struct list_head *itr;
	struct toyfs_iblkref *src_iblkref, *dst_iblkref;
	struct zus_inode *src_zi = src_tii->zii.zi;
	struct zus_inode *dst_zi = dst_tii->zii.zi;
	struct list_head *src_iblkrefs = &src_tii->ti->ti.reg.r_iblkrefs;
	struct list_head *dst_iblkrefs = &dst_tii->ti->ti.reg.r_iblkrefs;

	_drop_range(dst_tii, 0);

	_sbi_lock(dst_tii->sbi);
	itr = src_iblkrefs->next;
	while (itr != src_iblkrefs) {
		src_iblkref = container_of(itr, struct toyfs_iblkref, head);
		itr = itr->next;

		dst_iblkref = _consume_iblkref(dst_tii->sbi);
		if (!dst_iblkref) {
			_sbi_unlock(dst_tii->sbi);
			return -ENOSPC;
		}
		dst_iblkref->off = src_iblkref->off;
		dst_iblkref->dblkref = src_iblkref->dblkref;
		dst_iblkref->dblkref->refcnt++;
		list_add_tail(&dst_iblkref->head, dst_iblkrefs);
		dst_zi->i_blocks++;
	}
	_sbi_unlock(dst_tii->sbi);
	dst_zi->i_size = src_zi->i_size;
	return 0;
}

static struct toyfs_page *
_unique_page(struct toyfs_sb_info *sbi, struct toyfs_iblkref *iblkref)
{
	struct toyfs_page *page, *new_page;
	struct toyfs_dblkref *dblkref = iblkref->dblkref;

	page = toyfs_bn2page(sbi, dblkref->bn);
	if (dblkref->refcnt > 1) {
		dblkref = _consume_dblkref(sbi);
		if (!dblkref)
			return NULL;
		new_page = toyfs_bn2page(sbi, dblkref->bn);
		_copy_page(new_page, page);

		iblkref->dblkref->refcnt--;
		iblkref->dblkref = dblkref;
		page = new_page;
	}
	return page;
}

static void _share_page(struct toyfs_sb_info *sbi,
			       struct toyfs_iblkref *src_iblkref,
			       struct toyfs_iblkref *dst_iblkref)
{
	struct toyfs_dblkref *dblkref = dst_iblkref->dblkref;

	if (dblkref) {
		dblkref->refcnt--;
		if (!dblkref->refcnt)
			_free_dblkref(sbi, dblkref);
	}
	dblkref = dst_iblkref->dblkref = src_iblkref->dblkref;
	dblkref->refcnt++;
}

static bool _is_entire_page(loff_t src_off, loff_t dst_off, size_t len)
{
	return ((len == PAGE_SIZE) &&
		(_off_in_page(src_off) == 0) &&
		(_off_in_page(dst_off) == 0));
}

static int _clone_range(struct toyfs_inode_info *src_tii,
			struct toyfs_inode_info *dst_tii,
			loff_t src_off, loff_t dst_off, size_t len)
{
	size_t size;
	struct toyfs_page *dst_page;
	struct toyfs_iblkref *dst_iblkref, *src_iblkref;
	struct toyfs_sb_info *sbi = dst_tii->sbi;
	struct zus_inode *dst_zi = dst_tii->zii.zi;

	toyfs_assert(_is_entire_page(src_off, dst_off, len));
	src_iblkref = _fetch_iblkref(src_tii, src_off);
	dst_iblkref = _fetch_iblkref(dst_tii, dst_off);

	if (src_iblkref) {
		dst_iblkref = _require_iblkref(dst_tii, dst_off);
		if (!dst_iblkref)
			return -ENOSPC;
		_share_page(sbi, src_iblkref, dst_iblkref);
	} else {
		dst_iblkref = _fetch_iblkref(dst_tii, dst_off);
		if (!dst_iblkref)
			return 0;
		dst_page = _unique_page(sbi, dst_iblkref);
		if (!dst_page)
			return -ENOSPC;
		_assign_zeros(dst_page, _off_in_page(dst_off), len);
	}
	size = (size_t)dst_off + len;
	if (size > dst_zi->i_size)
		dst_zi->i_size = size;

	return 0;
}

static int
_clone_sub_file_range(struct toyfs_inode_info *src_tii,
		      struct toyfs_inode_info *dst_tii,
		      loff_t src_pos, loff_t dst_pos, size_t nbytes)
{
	int err;
	size_t src_len, dst_len, len;
	loff_t src_off, src_end, src_nxt;
	loff_t dst_off, dst_end, dst_nxt;

	src_off = src_pos;
	src_end = src_off + (loff_t)nbytes;
	dst_off = dst_pos;
	dst_end = dst_off + (loff_t)nbytes;
	while ((src_off < src_end) && (dst_off < dst_end)) {
		src_nxt = _next_page(src_off);
		src_len = _nbytes_in_range(src_off, src_nxt, src_end);

		dst_nxt = _next_page(dst_off);
		dst_len = _nbytes_in_range(dst_off, dst_nxt, dst_end);

		len = src_len < dst_len ? src_len : dst_len;
		err = _clone_range(src_tii, dst_tii, src_off, dst_off, len);
		if (err)
			return err;

		src_off += len;
		dst_off += len;
	}
	return 0;
}

static int toyfs_clone(struct zufs_ioc_clone *ioc_clone)
{
	struct toyfs_inode_info *src_tii = Z2II(ioc_clone->src_zus_ii);
	struct zus_inode *src_zi = src_tii->zii.zi;
	struct toyfs_inode_info *dst_tii = Z2II(ioc_clone->dst_zus_ii);
	struct zus_inode *dst_zi = dst_tii->zii.zi;
	const loff_t pos_in = (loff_t)ioc_clone->pos_in;
	const loff_t pos_out = (loff_t)ioc_clone->pos_out;
	const size_t len = (size_t)ioc_clone->len;

	DBG("clone: src_ino=%ld dst_ino=%ld pos_in=%ld pos_out=%ld len=%lu\n",
	    src_tii->ino, dst_tii->ino, pos_in, pos_out, len);

	if (!S_ISREG(src_zi->i_mode) || !S_ISREG(dst_zi->i_mode))
		return -ENOTSUP;

	if (src_tii == dst_tii)
		return 0;

	if (!pos_in && !len && !pos_out)
		return _clone_entire_file_range(src_tii, dst_tii);

	/* Follow XFS: only reflink if we're aligned to page boundaries */
	if (!_ispagealigned(pos_in, 0) || !_ispagealigned(pos_in, len) ||
	    !_ispagealigned(pos_out, 0) || !_ispagealigned(pos_out, len))
		return -ENOTSUP;

	return _clone_sub_file_range(src_tii, dst_tii, pos_in, pos_out, len);
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
	.clone = toyfs_clone,
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

static void toyfs_check_types(void)
{
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_pool_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_inodes_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_iblkrefs_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_dirents_page);
}

int toyfs_register_fs(int fd)
{
	toyfs_check_types();
	return zus_register_one(fd, &toyfs_zfi);
}
