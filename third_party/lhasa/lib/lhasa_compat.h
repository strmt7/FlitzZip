/*

Copyright (c) 2026, Efstratios Mitridis

Permission to use, copy, modify, and/or distribute this software
for any purpose with or without fee is hereby granted, provided
that the above copyright notice and this permission notice appear
in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 */

#ifndef LHASA_COMPAT_H
#define LHASA_COMPAT_H

#include <stddef.h>
#include <stdlib.h>

/* Purpose: Allocate zero-filled storage while normalizing zero-byte requests.
 * Inputs: len is the requested byte count.
 * Outputs: Returns caller-owned storage or NULL on failure. */
static void *lhasa_allocate(size_t len)
{
	if (len == 0) {
		len = 1;
	}
	return calloc(1, len);
}

/* Purpose: Add two byte counts with overflow detection.
 * Inputs: left/right are byte counts and out receives the sum.
 * Outputs: Returns non-zero on success, zero on overflow. */
static int lhasa_add_size(size_t left, size_t right, size_t *out)
{
	if (right > ((size_t) -1) - left) {
		return 0;
	}
	*out = left + right;
	return 1;
}

/* Purpose: Copy a non-overlapping byte span without calling banned C runtime copy APIs.
 * Inputs: dest/src point to valid spans of at least len bytes.
 * Outputs: Writes len bytes into dest. */
static void lhasa_bytes_copy(void *dest, const void *src, size_t len)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
	size_t i;

	for (i = 0; i < len; ++i) {
		d[i] = s[i];
	}
}

/* Purpose: Move a byte span while preserving overlapping-source semantics.
 * Inputs: dest/src point to valid spans of at least len bytes.
 * Outputs: Writes len bytes into dest without corrupting overlapping ranges. */
static void lhasa_bytes_move(void *dest, const void *src, size_t len)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
	size_t i;

	if (d == s || len == 0) {
		return;
	}
	if (d < s || d >= s + len) {
		for (i = 0; i < len; ++i) {
			d[i] = s[i];
		}
	} else {
		for (i = len; i > 0; --i) {
			d[i - 1] = s[i - 1];
		}
	}
}

/* Purpose: Fill a byte span without calling banned C runtime fill APIs.
 * Inputs: dest points to at least len bytes and value is converted to unsigned char.
 * Outputs: Writes len copies of value into dest. */
static void lhasa_bytes_set(void *dest, int value, size_t len)
{
	unsigned char *d = dest;
	size_t i;

	for (i = 0; i < len; ++i) {
		d[i] = (unsigned char) value;
	}
}

/* Purpose: Return the number of bytes before the terminating zero byte.
 * Inputs: text points to a valid zero-terminated string.
 * Outputs: Returns the string length in bytes. */
static size_t lhasa_string_length(const char *text)
{
	size_t len;

	for (len = 0; text[len] != '\0'; ++len) {
	}
	return len;
}

/* Purpose: Compare two zero-terminated strings for exact equality.
 * Inputs: left/right point to valid zero-terminated strings.
 * Outputs: Returns non-zero when both strings contain identical bytes. */
static int lhasa_string_equal(const char *left, const char *right)
{
	size_t i;

	for (i = 0; left[i] != '\0' && right[i] != '\0'; ++i) {
		if (left[i] != right[i]) {
			return 0;
		}
	}
	return left[i] == right[i];
}

/* Purpose: Allocate and copy a null-terminated string without calling the nonstandard runtime duplicate API.
 * Inputs: text points to a valid null-terminated string.
 * Outputs: Returns a heap string owned by the caller or NULL on allocation failure. */
static char *lhasa_string_duplicate(const char *text)
{
	size_t len;
	char *result;

	len = lhasa_string_length(text);
	result = lhasa_allocate(len + 1);
	if (result == NULL) {
		return NULL;
	}
	lhasa_bytes_copy(result, text, len);
	result[len] = '\0';
	return result;
}

#endif /* #ifndef LHASA_COMPAT_H */
