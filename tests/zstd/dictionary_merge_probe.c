/* This test bridge is appended after the exact generated dictItem declaration
 * and isIncluded/merger source region. It is not a second merge algorithm. */
#include "dictionary_merge_probe.h"

/* Purpose: Marshal a bounded fixture into the private production dictionary merger.
 * Inputs: See dictionary_merge_probe.h; entries and source spans remain caller-owned.
 * Outputs: Returns the exact merger result and all table fields; rejects invalid fixture shape. */
uint32_t sz_dictionary_merge(sz_dictionary_item* items, size_t count, sz_dictionary_item item, uint32_t skip,
                             const void* buffer) {
    dictItem table[16];
    dictItem candidate;
    U32 result;
    size_t index;
    if (items == NULL || buffer == NULL || count == 0 || count > 16 || items[0].position != count || item.length == 0 ||
        skip >= count) {
        return UINT32_MAX;
    }
    for (index = 0; index < count; ++index) {
        table[index].pos = items[index].position;
        table[index].length = items[index].length;
        table[index].savings = items[index].savings;
    }
    candidate.pos = item.position;
    candidate.length = item.length;
    candidate.savings = item.savings;
    result = ZDICT_tryMerge(table, candidate, skip, buffer);
    for (index = 0; index < count; ++index) {
        items[index].position = table[index].pos;
        items[index].length = table[index].length;
        items[index].savings = table[index].savings;
    }
    return result;
}
