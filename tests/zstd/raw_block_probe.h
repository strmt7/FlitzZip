#ifndef SUPERZIP_ZSTD_RAW_BLOCK_PROBE_H
#define SUPERZIP_ZSTD_RAW_BLOCK_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Purpose: Invoke the canonical generated raw writer through a narrow test bridge.
 * Inputs: destination/source borrow capacity/bytes; last_block has the upstream meaning.
 * Outputs: Returns the exact private helper's result without importing unrelated codec state. */
size_t sz_zstd_no_compress_block(void* destination, size_t capacity, const void* source, size_t bytes,
                                 unsigned last_block);

#ifdef __cplusplus
}
#endif

#endif
