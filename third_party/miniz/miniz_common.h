#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "miniz_export.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define MZ_INTERNAL_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MZ_INTERNAL_INLINE static __inline__ __attribute__((__always_inline__))
#else
#define MZ_INTERNAL_INLINE static inline
#endif

/* ------------------- Types and macros */
typedef unsigned char mz_uint8;
typedef int16_t mz_int16;
typedef uint16_t mz_uint16;
typedef uint32_t mz_uint32;
typedef uint32_t mz_uint;
typedef int64_t mz_int64;
typedef uint64_t mz_uint64;
typedef int mz_bool;

#define MZ_FALSE (0)
#define MZ_TRUE (1)

/* Works around MSVC's spammy "warning C4127: conditional expression is constant" message. */
#ifdef _MSC_VER
#define MZ_MACRO_END while (0, 0)
#else
#define MZ_MACRO_END while (0)
#endif

#ifdef MINIZ_NO_STDIO
#define MZ_FILE void *
#else
#include <stdio.h>
#define MZ_FILE FILE
#endif /* #ifdef MINIZ_NO_STDIO */

#ifdef MINIZ_NO_TIME
typedef struct mz_dummy_time_t_tag
{
    mz_uint32 m_dummy1;
    mz_uint32 m_dummy2;
} mz_dummy_time_t;
#define MZ_TIME_T mz_dummy_time_t
#else
#define MZ_TIME_T time_t
#endif

#define MZ_ASSERT(x) assert(x)

/* Purpose: Allocate a bounded heap block for miniz internals.
   Inputs: `size` is the requested byte count.
   Outputs: Returns a heap pointer, or NULL when allocation fails. */
MZ_INTERNAL_INLINE void *mz_internal_alloc(size_t size)
{
    const size_t effective_size = size ? size : 1U;
#if defined(_WIN32)
    return HeapAlloc(GetProcessHeap(), 0, effective_size);
#else
#error "SuperZip's patched miniz allocator is Windows-only."
#endif
}

/* Purpose: Release a heap block allocated by mz_internal_alloc or mz_internal_resize.
   Inputs: `address` is a pointer previously returned by the miniz allocator, or NULL.
   Outputs: Releases the block when non-NULL. */
MZ_INTERNAL_INLINE void mz_internal_release(void *address)
{
    if (!address)
        return;
#if defined(_WIN32)
    (void)HeapFree(GetProcessHeap(), 0, address);
#else
#error "SuperZip's patched miniz allocator is Windows-only."
#endif
}

/* Purpose: Resize a heap block while preserving existing contents.
   Inputs: `address` is an existing allocation or NULL; `size` is the new byte count.
   Outputs: Returns the resized pointer, or NULL when allocation fails. */
MZ_INTERNAL_INLINE void *mz_internal_resize(void *address, size_t size)
{
    const size_t effective_size = size ? size : 1U;
#if defined(_WIN32)
    if (!address)
        return HeapAlloc(GetProcessHeap(), 0, effective_size);
    return HeapReAlloc(GetProcessHeap(), 0, address, effective_size);
#else
#error "SuperZip's patched miniz allocator is Windows-only."
#endif
}

/* Purpose: Copy bytes between non-overlapping or overlapping internal buffers.
   Inputs: `dst` and `src` are byte buffers, `dst_capacity` is the known destination
   byte capacity, and `count` is the requested byte count.
   Outputs: Returns MZ_TRUE on success and MZ_FALSE when arguments are invalid. */
MZ_INTERNAL_INLINE mz_bool mz_copy_bytes(void *dst, size_t dst_capacity, const void *src, size_t count)
{
    mz_uint8 *d = (mz_uint8 *)dst;
    const mz_uint8 *s = (const mz_uint8 *)src;
    size_t i;
    if (!count)
        return MZ_TRUE;
    if (!d || !s || (count > dst_capacity))
        return MZ_FALSE;
    if ((d > s) && (d < s + count))
    {
        for (i = count; i > 0; --i)
            d[i - 1] = s[i - 1];
    }
    else
    {
        for (i = 0; i < count; ++i)
            d[i] = s[i];
    }
    return MZ_TRUE;
}

/* Purpose: Fill an internal byte buffer with a repeated value.
   Inputs: `dst` is the destination buffer, `value` is the byte to write, and
   `count` is the number of bytes.
   Outputs: Returns MZ_TRUE on success and MZ_FALSE when arguments are invalid. */
MZ_INTERNAL_INLINE mz_bool mz_fill_bytes(void *dst, mz_uint8 value, size_t count)
{
    mz_uint8 *d = (mz_uint8 *)dst;
    size_t i;
    if (!count)
        return MZ_TRUE;
    if (!d)
        return MZ_FALSE;
    for (i = 0; i < count; ++i)
        d[i] = value;
    return MZ_TRUE;
}

/* Purpose: Compute a bounded C-string length for archive metadata.
   Inputs: `text` is a NUL-terminated string and `max_len` caps scanning.
   Outputs: Returns the length before NUL, capped at `max_len`. */
MZ_INTERNAL_INLINE size_t mz_cstr_len_bound(const char *text, size_t max_len)
{
    size_t len = 0;
    if (!text)
        return 0;
    while ((len < max_len) && text[len])
        ++len;
    return len;
}

#ifdef MINIZ_NO_MALLOC
#define MZ_MALLOC(x) NULL
#define MZ_FREE(x) (void)x, ((void)0)
#define MZ_REALLOC(p, x) NULL
#else
#define MZ_MALLOC(x) mz_internal_alloc(x)
#define MZ_FREE(x) mz_internal_release(x)
#define MZ_REALLOC(p, x) mz_internal_resize(p, x)
#endif

#define MZ_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MZ_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MZ_CLEAR_OBJ(obj) (void)mz_fill_bytes(&(obj), 0, sizeof(obj))
#define MZ_CLEAR_ARR(obj) (void)mz_fill_bytes((obj), 0, sizeof(obj))
#define MZ_CLEAR_PTR(obj) (void)mz_fill_bytes((obj), 0, sizeof(*obj))

#if MINIZ_USE_UNALIGNED_LOADS_AND_STORES && MINIZ_LITTLE_ENDIAN
#define MZ_READ_LE16(p) *((const mz_uint16 *)(p))
#define MZ_READ_LE32(p) *((const mz_uint32 *)(p))
#else
#define MZ_READ_LE16(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U))
#define MZ_READ_LE32(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U) | ((mz_uint32)(((const mz_uint8 *)(p))[2]) << 16U) | ((mz_uint32)(((const mz_uint8 *)(p))[3]) << 24U))
#endif

#define MZ_READ_LE64(p) (((mz_uint64)MZ_READ_LE32(p)) | (((mz_uint64)MZ_READ_LE32((const mz_uint8 *)(p) + sizeof(mz_uint32))) << 32U))

#ifdef __cplusplus
extern "C"
{
#endif

    extern MINIZ_EXPORT void *miniz_def_alloc_func(void *opaque, size_t items, size_t size);
    extern MINIZ_EXPORT void miniz_def_free_func(void *opaque, void *address);
    extern MINIZ_EXPORT void *miniz_def_realloc_func(void *opaque, void *address, size_t items, size_t size);

#define MZ_UINT16_MAX (0xFFFFU)
#define MZ_UINT32_MAX (0xFFFFFFFFU)

#ifdef __cplusplus
}
#endif
