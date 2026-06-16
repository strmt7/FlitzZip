/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/* This file provides common libc dependencies that zstd requires.
 * The purpose is to allow replacing this file with a custom implementation
 * to compile zstd without libc support.
 */

/* Need:
 * NULL
 * INT_MAX
 * UINT_MAX
 * ZSTD_memcpy()
 * ZSTD_memset()
 * ZSTD_memmove()
 */
#ifndef ZSTD_DEPS_COMMON
#define ZSTD_DEPS_COMMON

/* Even though we use qsort_r only for the dictionary builder, the macro
 * _GNU_SOURCE has to be declared *before* the inclusion of any standard
 * header and the script 'combine.sh' combines the whole zstd source code
 * in a single file.
 */
#if defined(__linux) || defined(__linux__) || defined(linux) || defined(__gnu_linux__) || \
    defined(__CYGWIN__) || defined(__MSYS__)
#if !defined(_GNU_SOURCE) && !defined(__ANDROID__) /* NDK doesn't ship qsort_r(). */
#define _GNU_SOURCE
#endif
#endif

#include <limits.h>
#include <stddef.h>
#if defined(__GNUC__) && __GNUC__ >= 4
# define ZSTD_memcpy(d,s,l) __builtin_memcpy((d),(s),(l))
# define ZSTD_memmove(d,s,l) __builtin_memmove((d),(s),(l))
# define ZSTD_memset(p,v,l) __builtin_memset((p),(v),(l))
#else
static void* superzip_zstd_copy_bytes(void* dst, const void* src, size_t len)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (len-- != 0) {
        *d++ = *s++;
    }
    return dst;
}
static void* superzip_zstd_move_bytes(void* dst, const void* src, size_t len)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d <= s) {
        while (len-- != 0) {
            *d++ = *s++;
        }
    } else {
        d += len;
        s += len;
        while (len-- != 0) {
            *--d = *--s;
        }
    }
    return dst;
}
static void* superzip_zstd_fill_bytes(void* dst, int value, size_t len)
{
    unsigned char* d = (unsigned char*)dst;
    while (len-- != 0) {
        *d++ = (unsigned char)value;
    }
    return dst;
}
# define ZSTD_memcpy(d,s,l) superzip_zstd_copy_bytes((d),(s),(l))
# define ZSTD_memmove(d,s,l) superzip_zstd_move_bytes((d),(s),(l))
# define ZSTD_memset(p,v,l) superzip_zstd_fill_bytes((p),(v),(l))
#endif

#endif /* ZSTD_DEPS_COMMON */

/* Need:
 * ZSTD_malloc()
 * ZSTD_free()
 * ZSTD_calloc()
 */
#ifdef ZSTD_DEPS_NEED_MALLOC
#ifndef ZSTD_DEPS_MALLOC
#define ZSTD_DEPS_MALLOC

#include <stdlib.h>

static void* superzip_zstd_alloc(size_t size)
{
    return size == 0 ? NULL : calloc(1, size);
}
static void* superzip_zstd_alloc_array(size_t count, size_t size)
{
    if (count != 0 && size > ((size_t)-1) / count) {
        return NULL;
    }
    return count == 0 || size == 0 ? NULL : calloc(count, size);
}
static void superzip_zstd_release(void* ptr)
{
    free(ptr);
}

#define ZSTD_malloc(s) superzip_zstd_alloc(s)
#define ZSTD_calloc(n,s) superzip_zstd_alloc_array((n), (s))
#define ZSTD_free(p) superzip_zstd_release(p)

#endif /* ZSTD_DEPS_MALLOC */
#endif /* ZSTD_DEPS_NEED_MALLOC */

/*
 * Provides 64-bit math support.
 * Need:
 * U64 ZSTD_div64(U64 dividend, U32 divisor)
 */
#ifdef ZSTD_DEPS_NEED_MATH64
#ifndef ZSTD_DEPS_MATH64
#define ZSTD_DEPS_MATH64

#define ZSTD_div64(dividend, divisor) ((dividend) / (divisor))

#endif /* ZSTD_DEPS_MATH64 */
#endif /* ZSTD_DEPS_NEED_MATH64 */

/* Need:
 * assert()
 */
#ifdef ZSTD_DEPS_NEED_ASSERT
#ifndef ZSTD_DEPS_ASSERT
#define ZSTD_DEPS_ASSERT

#include <assert.h>

#endif /* ZSTD_DEPS_ASSERT */
#endif /* ZSTD_DEPS_NEED_ASSERT */

/* Need:
 * ZSTD_DEBUG_PRINT()
 */
#ifdef ZSTD_DEPS_NEED_IO
#ifndef ZSTD_DEPS_IO
#define ZSTD_DEPS_IO

#define ZSTD_DEBUG_PRINT(...) ((void)0)

#endif /* ZSTD_DEPS_IO */
#endif /* ZSTD_DEPS_NEED_IO */

/* Only requested when <stdint.h> is known to be present.
 * Need:
 * intptr_t
 */
#ifdef ZSTD_DEPS_NEED_STDINT
#ifndef ZSTD_DEPS_STDINT
#define ZSTD_DEPS_STDINT

#include <stdint.h>

#endif /* ZSTD_DEPS_STDINT */
#endif /* ZSTD_DEPS_NEED_STDINT */
