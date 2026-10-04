#include "huffman_table_probe.h"

/* Purpose: Exercise the exact generated Huffman fill using compatible typed local entries.
 * Inputs: Bounded fixture records including canaries and live complete rank/symbol arrays.
 * Outputs: Native result and exact observed records; rejects incomplete fixture extents before access. */
size_t sz_huf_fill_x1(sz_huf_x1_entry* records, size_t stored, size_t capacity, const unsigned char* symbols,
                      size_t symbol_count, const uint32_t* ranks, size_t rank_count, unsigned table_log) {
    HUF_DEltX1 entries[1026];
    HUF_X1TableView view;
    size_t index, result;
    if (records == NULL || stored > 1026 || capacity > stored || table_log > HUF_TABLELOG_MAX ||
        rank_count <= table_log || ranks == NULL)
        return ERROR(parameter_outOfBound);
    for (index = 0; index < stored; ++index) {
        entries[index].nbBits = records[index].nb_bits;
        entries[index].byte = records[index].symbol;
    }
    view.entries = entries;
    view.capacity = capacity;
    result = HUF_fillX1Table(view, symbols, symbol_count, ranks, table_log);
    for (index = 0; index < stored; ++index) {
        records[index].nb_bits = entries[index].nbBits;
        records[index].symbol = entries[index].byte;
    }
    return result;
}
