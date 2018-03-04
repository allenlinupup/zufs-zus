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
#include <limits.h>

#include "zus.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x_)	( sizeof(x_) / sizeof(x_[0]) )
#endif
#ifndef MAKESTR
#define MAKESTR(x_)	#x_
#endif
#ifndef STR
#define STR(x_)		MAKESTR(x_)
#endif


#define TOYFS_NULL_INO		(0)
#define TOYFS_ROOT_INO		(1)


struct toyfs_page {
	uint8_t dat[PAGE_SIZE];
};

struct toyfs_pool {
	pthread_mutex_t mutex;
	union toyfs_pool_page *pages;
	struct list_head free_dblkrefs;
	struct list_head free_iblkrefs;
	struct list_head free_dirents;
	struct list_head free_inodes;
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
};


struct toyfs_super_block {
	union toyfs_super_block_part part1;
	union toyfs_super_block_part part2;
};

struct toyfs_sb_info {
	struct zus_sb_info s_zus_sbi;
	struct statvfs s_statvfs;
	pthread_mutex_t s_mutex;
	struct toyfs_pool s_pool;
	struct toyfs_itable s_itable;
	struct toyfs_inode_info *s_root;
	ino_t s_top_ino;
};


struct toyfs_inode_dir {
	struct list_head d_childs;
	size_t d_ndentry;
	loff_t d_off_max;
};

struct toyfs_inode_reg {
	struct list_head r_iblkrefs;
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
};

union toyfs_inode_head {
	struct list_head head;
	struct toyfs_inode inode;
};

struct toyfs_inode_info {
	struct zus_inode_info zii;
	struct toyfs_sb_info *sbi;
	struct toyfs_inode *ti;
	struct toyfs_inode_info *next;
	ino_t ino;
	unsigned long imagic;
};

struct toyfs_dirent {
	struct list_head d_head;
	loff_t  d_off;
	ino_t   d_ino;
	size_t  d_nlen;
	mode_t 	d_type;
	char    d_name[ZUFS_NAME_LEN + 1]; /* TODO: Use variable size */
};

struct toyfs_dblkref {
	struct list_head head;
	size_t refcnt;
	size_t bn;
};

struct toyfs_iblkref {
	struct list_head head;
	struct toyfs_dblkref *dblkref;
	loff_t off;
};

int toyfs_register_fs(int fd);

#endif /* TOYFS_H_*/
