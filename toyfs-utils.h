/*
 * toyfs-utils.h - Common utilities for toyfs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */
#ifndef TOYFS_UTILS_H_
#define TOYFS_UTILS_H_

#include <sched.h>

#define TOYFS_KILO      (1ULL << 10)
#define TOYFS_MEGA      (1ULL << 20)
#define TOYFS_GIGA      (1ULL << 30)

#define TOYFS_NORETURN	__attribute__ ((__noreturn__))

#define TOYFS_STATICASSERT(expr)	_Static_assert(expr, #expr)
#define TOYFS_STATICASSERT_EQ(a, b)	TOYFS_STATICASSERT(a == b)
#define TOYFS_STATICASSERT_LE(a, b)	TOYFS_STATICASSERT(a <= b)

#define TOYFS_ARRAY_SIZE(x_)	(sizeof(x_)/sizeof(x_[0]))
#define TOYFS_MAKESTR(x_)	#x_
#define TOYFS_STR(x_)		TOYFS_MAKESTR(x_)

#define toyfs_container_of(ptr, type, member) \
	(type *)((void *)((char *)ptr - offsetof(type, member)))

#define toyfs_likely(x_)	__builtin_expect(!!(x_), 1)
#define toyfs_unlikely(x_)	__builtin_expect(!!(x_), 0)
#define toyfs_unused(x_)	((void)(x_))

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/

#define toyfs_info_(fmt, ...)

#define toyfs_info(fmt, ...) \
	INFO("[%d] " fmt "\n", sched_getcpu(), __VA_ARGS__)
#define toyfs_error(fmt, ...) \
	ERROR("[%d] " fmt "\n", sched_getcpu(), __VA_ARGS__)
#define toyfs_panic(fmt, ...) \
	toyfs_panicf(__FILE__, __LINE__, fmt, __VA_ARGS__)
#define toyfs_panic_if_err(err, msg) \
	do { if (err) toyfs_panic("%s: %d", msg, err); } while (0)
#define toyfs_assert(cond) \
	do { if (!(cond)) toyfs_panic("assert failed: %s", #cond); } while (0)

#endif /* TOYFS_UTILS_H_*/

