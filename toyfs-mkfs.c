/*
 * toyfs-mkfs.c - A mkfs utility for the toyfs file-system
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <uuid/uuid.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"

#define NSEC_PER_SEC	1000000000L

static void timespec_to_mt(uint64_t *mt, struct timespec *t)
{
	*mt = t->tv_sec * NSEC_PER_SEC + t->tv_nsec;
}

static uint16_t const crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

static uint16_t crc16_byte(uint16_t crc, const uint8_t data)
{
	return (crc >> 8) ^ crc16_table[(crc ^ data) & 0xff];
}

static uint16_t crc16(uint16_t crc, uint8_t const *buffer, size_t len)
{
	while (len--)
		crc = crc16_byte(crc, *buffer++);
	return crc;
}

static uint16_t toyfs_calc_csum(struct zufs_dev_table *dev_table)
{
#define u64 uint64_t

	uint32_t n = ZUFS_SB_STATIC_SIZE(dev_table) - sizeof(dev_table->s_sum);

	return crc16(~0, (__u8 *)&dev_table->s_version, n);
}

static int toyfs_open_blkdev(const char *path, loff_t *sz)
{
	int fd, err;
	size_t bdev_size = 0, min_size = 1UL << 20;
	struct stat st;

	fd = open(path, O_RDWR);
	if (fd <= 0)
		error(EXIT_FAILURE, -errno, "open failed: %s", path);

	err = fstat(fd, &st);
	if (err)
		error(EXIT_FAILURE, -errno, "fstat failed: %s", path);

	if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode))
		error(EXIT_FAILURE, -1, "not block or regualr file: %s", path);

	if (S_ISBLK(st.st_mode)) {
		err = ioctl(fd, BLKGETSIZE64, &bdev_size);
		if (err)
			error(EXIT_FAILURE, err,
			      "ioctl(BLKGETSIZE64) failed: %s", path);
		if (bdev_size < min_size)
			error(EXIT_FAILURE, 0,
			      "illegal device size: %s %lu", path, bdev_size);
		*sz = (loff_t)bdev_size;
	} else {
		if (st.st_size < (loff_t)min_size)
			error(EXIT_FAILURE, 0,
			      "illegal size: %s %ld", path, st.st_size);
		*sz = st.st_size;
	}
	printf("open device: %s size=%ld fd=%d\n", path, *sz, fd);
	return fd;
}

static void toyfs_close_blkdev(const char *path, int fd)
{
	printf("close device: %s fd=%d\n", path, fd);
	close(fd);
}

static void toyfs_fill_dev_table(struct zufs_dev_table *dev_table,
				 loff_t dev_size, const char *uu)
{
	int err;
	struct timespec now;
	uuid_t super_uuid, dev_uuid;
	struct zufs_dev_id *dev_id;

	uuid_generate(super_uuid);
	err = uuid_parse(uu, dev_uuid);
	if (err)
		error(EXIT_FAILURE, 0, "illegal uuid: %s", uu);

	memset(dev_table, 0, sizeof(*dev_table));
	memcpy(&dev_table->s_uuid, super_uuid, sizeof(dev_table->s_uuid));
	dev_table->s_version = (ZUFS_MAJOR_VERSION * ZUFS_MINORS_PER_MAJOR) +
			       ZUFS_MINOR_VERSION;
	dev_table->s_magic = ZUFS_SUPER_MAGIC;
	dev_table->s_flags = 0;
	dev_table->s_t1_blocks = pmem_o2p(dev_size);
	dev_table->s_dev_list.id_index = 0;
	dev_table->s_dev_list.t1_count = 1;

	dev_id = &dev_table->s_dev_list.dev_ids[0];
	memcpy(&dev_id->uuid, dev_uuid, sizeof(dev_id->uuid));
	dev_id->blocks = dev_table->s_t1_blocks;
	printf("device: uuid=%s blocks=%lu\n", uu, (size_t)dev_id->blocks);

	clock_gettime(CLOCK_REALTIME, &now);
	timespec_to_mt(((uint64_t *)&dev_table->s_wtime), &now);
	dev_table->s_sum = toyfs_calc_csum(dev_table);
}

static void toyfs_mirror_parts(struct toyfs_super_block *super_block)
{
	union toyfs_super_block_part *part1 = &super_block->part1;
	union toyfs_super_block_part *part2 = &super_block->part2;

	memcpy(part2, part1, sizeof(*part2));
}

static void
toyfs_write_super_block(int fd, struct toyfs_super_block *super_block)
{
	int err;
	loff_t off;

	off = lseek(fd, 0, SEEK_SET);
	if (off != 0)
		error(EXIT_FAILURE, -errno,
		      "failed to lseek to offset=%ld", off);

	err = write(fd, super_block, sizeof(*super_block));
	if (err != (int)sizeof(*super_block))
		error(EXIT_FAILURE, -errno, "failed to write super block");

	err = fsync(fd);
	if (err)
		error(EXIT_FAILURE, -errno, "failed to fsync");
}

static void toyfs_fill_root_inode(struct toyfs_inode *rooti)
{
	memset(rooti, 0, sizeof(*rooti));

	rooti->zi.i_ino = TOYFS_ROOT_INO;
	rooti->zi.i_nlink = 2;
	rooti->zi.i_size = 0;
	rooti->i_parent_ino = TOYFS_ROOT_INO;
	rooti->ti.dir.d_off_max = 2;
}

static void toyfs_write_root_inode(int fd, struct toyfs_inode *rooti)
{
	int err;
	loff_t off;

	off = lseek(fd, TOYFS_PAGE_SIZE, SEEK_SET);
	if (off != TOYFS_PAGE_SIZE)
		error(EXIT_FAILURE, -errno,
		      "failed to lseek to offset=%ld", off);

	err = write(fd, rooti, sizeof(*rooti));
	if (err != (int)sizeof(*rooti))
		error(EXIT_FAILURE, -errno, "failed to write root inode");

	err = fsync(fd);
	if (err)
		error(EXIT_FAILURE, -errno, "failed to fsync");
}


static struct toyfs_super_block g_super_block;
static struct toyfs_inode g_root_inode;

int main(int argc, char *argv[])
{
	int fd;
	loff_t dev_size = 0;
	struct toyfs_super_block *sb = &g_super_block;
	struct toyfs_inode *rooti = &g_root_inode;

	if (argc != 3)
		error(EXIT_FAILURE, -1, "usage: mkfs <device-path> <uuid>");

	fd = toyfs_open_blkdev(argv[1], &dev_size);
	toyfs_fill_dev_table(&sb->part1.dev_table, dev_size, argv[2]);
	toyfs_mirror_parts(sb);
	toyfs_fill_root_inode(rooti);
	toyfs_write_super_block(fd, sb);
	toyfs_write_root_inode(fd, rooti);
	toyfs_close_blkdev(argv[1], fd);
	return 0;
}
