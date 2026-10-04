#ifndef SUPERZIP_ZSTD_DICTIONARY_MERGE_PROBE_H
#define SUPERZIP_ZSTD_DICTIONARY_MERGE_PROBE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t position;
    uint32_t length;
    uint32_t savings;
} sz_dictionary_item;

#ifdef __cplusplus
extern "C" {
#endif

/* Purpose: Exercise the exact generated dictionary merger without exporting private product APIs.
 * Inputs: items owns 1-16 initialized entries, including the sentinel; item has nonzero length;
 * buffer owns the source spans plus the legacy trainer's 32-byte noise guard.
 * Outputs: Returns the production destination rank, or UINT32_MAX for an invalid probe request;
 * copies the complete mutated table back without changing the product implementation. */
uint32_t sz_dictionary_merge(sz_dictionary_item* items, size_t count, sz_dictionary_item item, uint32_t skip,
                             const void* buffer);

#ifdef __cplusplus
}
#endif

#endif
