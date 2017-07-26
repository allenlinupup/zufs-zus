/*
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>

#define FLUSH_ALIGN ((uintptr_t)64)

#define ALIGN_MASK	(FLUSH_ALIGN - 1)

#define CHUNK_SIZE	128 /* 16*8 */
#define CHUNK_SHIFT	7
#define CHUNK_MASK	(CHUNK_SIZE - 1)

#define DWORD_SIZE	4
#define DWORD_SHIFT	2
#define DWORD_MASK	(DWORD_SIZE - 1)

#define MOVNT_SIZE	16
#define MOVNT_MASK	(MOVNT_SIZE - 1)
#define MOVNT_SHIFT	4

#define MOVNT_THRESHOLD	256

static void flush_clflush(const void *addr, size_t len)
{
	uintptr_t uptr;

	/*
	 * Loop through cache-line-size (typically 64B) aligned chunks
	 * covering the given range.
	 */
	for (uptr = (uintptr_t)addr & ~(FLUSH_ALIGN - 1);
		uptr < (uintptr_t)addr + len; uptr += FLUSH_ALIGN)
		_mm_clflush((char *)uptr);
}

static void pmem_flush(const void *addr, size_t len)
{
	flush_clflush(addr, len);
}

static void * memmove_nodrain_movnt(void *pmemdest, const void *src, size_t len)
{
	__m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
	size_t i;
	__m128i *d;
	__m128i *s;
	void *dest1 = pmemdest;
	size_t cnt;

	if (len == 0 || src == pmemdest)
		return pmemdest;

	if (len < MOVNT_THRESHOLD) {
		memmove(pmemdest, src, len);
		pmem_flush(pmemdest, len);
		return pmemdest;
	}

	if ((uintptr_t)dest1 - (uintptr_t)src >= len) {
		/*
		 * Copy the range in the forward direction.
		 *
		 * This is the most common, most optimized case, used unless
		 * the overlap specifically prevents it.
		 */

		/* copy up to FLUSH_ALIGN boundary */
		cnt = (uint64_t)dest1 & ALIGN_MASK;
		if (cnt > 0) {
			cnt = FLUSH_ALIGN - cnt;

			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			uint8_t *d8 = (uint8_t *)dest1;
			const uint8_t *s8 = (uint8_t *)src;
			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(dest1, cnt);
			dest1 = (char *)dest1 + cnt;
			src = (char *)src + cnt;
			len -= cnt;
		}

		d = (__m128i *)dest1;
		s = (__m128i *)src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s);
			xmm1 = _mm_loadu_si128(s + 1);
			xmm2 = _mm_loadu_si128(s + 2);
			xmm3 = _mm_loadu_si128(s + 3);
			xmm4 = _mm_loadu_si128(s + 4);
			xmm5 = _mm_loadu_si128(s + 5);
			xmm6 = _mm_loadu_si128(s + 6);
			xmm7 = _mm_loadu_si128(s + 7);
			s += 8;
			_mm_stream_si128(d,	xmm0);
			_mm_stream_si128(d + 1,	xmm1);
			_mm_stream_si128(d + 2,	xmm2);
			_mm_stream_si128(d + 3,	xmm3);
			_mm_stream_si128(d + 4,	xmm4);
			_mm_stream_si128(d + 5, xmm5);
			_mm_stream_si128(d + 6,	xmm6);
			_mm_stream_si128(d + 7,	xmm7);
			d += 8;
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
				s++;
				d++;
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			cnt = len >> DWORD_SHIFT;
			int32_t *d32 = (int32_t *)d;
			int32_t *s32 = (int32_t *)s;
			for (i = 0; i < cnt; i++) {
				_mm_stream_si32(d32, *s32);
				d32++;
				s32++;
			}
			cnt = len & DWORD_MASK;
			uint8_t *d8 = (uint8_t *)d32;
			const uint8_t *s8 = (uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(d32, cnt);
		}
	} else {
		/*
		 * Copy the range in the backward direction.
		 *
		 * This prevents overwriting source data due to an
		 * overlapped destination range.
		 */

		dest1 = (char *)dest1 + len;
		src = (char *)src + len;

		cnt = (uint64_t)dest1 & ALIGN_MASK;
		if (cnt > 0) {
			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			uint8_t *d8 = (uint8_t *)dest1;
			const uint8_t *s8 = (uint8_t *)src;
			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
			dest1 = (char *)dest1 - cnt;
			src = (char *)src - cnt;
			len -= cnt;
		}

		d = (__m128i *)dest1;
		s = (__m128i *)src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s - 1);
			xmm1 = _mm_loadu_si128(s - 2);
			xmm2 = _mm_loadu_si128(s - 3);
			xmm3 = _mm_loadu_si128(s - 4);
			xmm4 = _mm_loadu_si128(s - 5);
			xmm5 = _mm_loadu_si128(s - 6);
			xmm6 = _mm_loadu_si128(s - 7);
			xmm7 = _mm_loadu_si128(s - 8);
			s -= 8;
			_mm_stream_si128(d - 1, xmm0);
			_mm_stream_si128(d - 2, xmm1);
			_mm_stream_si128(d - 3, xmm2);
			_mm_stream_si128(d - 4, xmm3);
			_mm_stream_si128(d - 5, xmm4);
			_mm_stream_si128(d - 6, xmm5);
			_mm_stream_si128(d - 7, xmm6);
			_mm_stream_si128(d - 8, xmm7);
			d -= 8;
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				d--;
				s--;
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			cnt = len >> DWORD_SHIFT;
			int32_t *d32 = (int32_t *)d;
			int32_t *s32 = (int32_t *)s;
			for (i = 0; i < cnt; i++) {
				d32--;
				s32--;
				_mm_stream_si32(d32, *s32);
			}

			cnt = len & DWORD_MASK;
			uint8_t *d8 = (uint8_t *)d32;
			const uint8_t *s8 = (uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
		}
	}

	/* serialize non-temporal store instructions */
	/* also ensure CLWB or CLFLUSHOPT completes */
	_mm_sfence();

	return pmemdest;
}

void * pmem_memcpy_persist(void *pmemdest, const void *src, size_t len)
{
	memmove_nodrain_movnt(pmemdest, src, len);

	return pmemdest;
}
