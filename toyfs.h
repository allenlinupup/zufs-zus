/*
 * toyfs.h - The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */
#ifndef TOYFS_H_
#define TOYFS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <uuid/uuid.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "zus.h"

#define TOYFS_NULL_INO		(0)
#define TOYFS_ROOT_INO		(1)
#define TOYFS_PAGE_SHIFT	(PAGE_SHIFT)
#define TOYFS_PAGE_SIZE		(PAGE_SIZE) /* 4K */
#define TOYFS_NAME_MAX		(255)
#define TOYFS_ISIZE_MAX		(1ULL << 50)
#define TOYFS_DOFF_MAX		(1LL << 40)
#define TOYFS_IMAGIC		(0x11E11F5)

#define TOYFS_ALIGNED	__attribute__ ((__aligned__))
#define TOYFS_PACKED	__attribute__ ((__packed__))


struct toyfs_page {
	uint8_t dat[TOYFS_PAGE_SIZE];

} TOYFS_ALIGNED;

struct toyfs_pool {
	pthread_mutex_t mutex;
	union toyfs_pool_page *pages;
	struct list_head bfree;
	struct list_head dfree;
	struct list_head ifree;
	void	*mem;
	size_t	msz;
	bool 	pmem;
};

struct toyfs_itable {
	pthread_mutex_t mutex;
	size_t icount;
	struct toyfs_inode_info *imap[33377]; /* TODO: Variable size */
};


union toyfs_super_block_part {
	struct zufs_dev_table dev_table;
	uint8_t reserved[ZUFS_SB_SIZE];
} TOYFS_PACKED;


struct toyfs_super_block {
	union toyfs_super_block_part part1;
	union toyfs_super_block_part part2;

} TOYFS_PACKED;

struct toyfs_sb_info {
	struct zus_sb_info s_zus_sbi;
	struct statvfs s_statvfs;
	pthread_mutex_t s_mutex;
	struct toyfs_pool s_pool;
	struct toyfs_itable s_itable;
	struct toyfs_inode_info *s_root;
	ino_t s_top_ino;

} TOYFS_ALIGNED;


struct toyfs_inode_dir {
	struct list_head d_childs;
	size_t d_ndentry;
	loff_t d_off_max;
};

struct toyfs_inode_reg {
	struct list_head r_blkrefs;
	ino_t r_first_parent;
};

union toyfs_inode_symlnk {
	struct toyfs_page *sl_long;
};

struct toyfs_inode {
	struct zus_inode zi;
	ino_t i_parent_ino;
	union {
		struct toyfs_inode_dir dir;
		struct toyfs_inode_reg reg;
		union toyfs_inode_symlnk symlnk;
		uint8_t align[56];
	} ti;

} TOYFS_PACKED;


union toyfs_inode_head {
	struct list_head head;
	struct toyfs_inode inode;

} TOYFS_ALIGNED;

struct toyfs_inode_info {
	struct zus_inode_info zii;
	struct toyfs_sb_info *sbi;
	struct toyfs_inode *ti;
	struct toyfs_inode_info *next;
	ino_t ino;
	unsigned long imagic;

} TOYFS_ALIGNED;

struct toyfs_dirent {
	struct list_head d_head;
	loff_t  d_off;
	ino_t   d_ino;
	size_t  d_nlen;
	mode_t 	d_type;
	char    d_name[TOYFS_NAME_MAX + 1]; /* TODO: Use variable size */

} TOYFS_ALIGNED;


struct toyfs_blkref {
	struct list_head b_head;
	loff_t b_off;
	size_t b_bn;

} TOYFS_ALIGNED;

int toyfs_register_fs(int fd);

#endif /* TOYFS_H_*/
