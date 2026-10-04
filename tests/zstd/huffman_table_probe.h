#ifndef SUPERZIP_HUFFMAN_TABLE_PROBE_H
#define SUPERZIP_HUFFMAN_TABLE_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    unsigned char nb_bits;
    unsigned char symbol;
} sz_huf_x1_entry;
/* Purpose: Observe the canonical typed fill without aliasing fixture storage.
 * Inputs: Live bounded records, logical capacity, symbol/rank arrays and table log.
 * Outputs: Returns the canonical result and copies all records, including canaries, back. */
size_t sz_huf_fill_x1(sz_huf_x1_entry* records, size_t stored, size_t capacity, const unsigned char* symbols,
                      size_t symbol_count, const uint32_t* ranks, size_t rank_count, unsigned table_log);
#ifdef __cplusplus
}
#endif
#endif
