/* Dictionary-merger role separation for pinned Zstandard 1.5.7.
 * The enclosing generated source retains its upstream license notice. */

/* Purpose: Restore savings order after a selected item grows, preserving equal-savings order.
 * Inputs: table owns its sentinel and sorted entries; candidateRank selects the already updated item.
 * Outputs: Moves lower-savings predecessors right and returns the final rank of merged; keeps table size. */
static U32 ZDICT_rankMergedItem(dictItem* table, U32 candidateRank, dictItem merged) {
    U32 rank = candidateRank;
    assert(rank >= 1 && rank < table[0].pos);
    while (rank > 1 && table[rank - 1].savings < merged.savings) {
        table[rank] = table[rank - 1];
        --rank;
    }
    table[rank] = merged;
    return rank;
}

/* Purpose: Find and merge an overlapping dictionary item, then restore its savings rank where required.
 * Inputs: table owns a sorted sentinel-indexed item list; elt has nonzero length;
 * buffer owns every item span and the trainer's noisy guard; eltNbToSkip identifies an excluded entry or zero.
 * Outputs: Returns the merged item's destination index or zero, preserving overlap arithmetic and candidate order. */
static U32 ZDICT_tryMerge(dictItem* table, dictItem elt, U32 eltNbToSkip, const void* buffer) {
    const U32 tableSize = table->pos;
    const U32 eltEnd = elt.pos + elt.length;
    const char* const buf = (const char*)buffer;

    /* tail overlap */
    U32 u;
    for (u = 1; u < tableSize; u++) {
        if (u == eltNbToSkip)
            continue;
        if ((table[u].pos > elt.pos) && (table[u].pos <= eltEnd)) { /* overlap, existing > new */
            /* append */
            U32 const addedLength = table[u].pos - elt.pos;
            table[u].length += addedLength;
            table[u].pos = elt.pos;
            table[u].savings += elt.savings * addedLength / elt.length; /* rough approx */
            table[u].savings += elt.length / 8;                         /* rough approx bonus */
            return ZDICT_rankMergedItem(table, u, table[u]);
        }
    }

    /* front overlap */
    for (u = 1; u < tableSize; u++) {
        if (u == eltNbToSkip)
            continue;

        if ((table[u].pos + table[u].length >= elt.pos) && (table[u].pos < elt.pos)) { /* overlap, existing < new */
            /* append */
            int const addedLength = (int)eltEnd - (int)(table[u].pos + table[u].length);
            table[u].savings += elt.length / 8; /* rough approx bonus */
            if (addedLength > 0) {              /* otherwise, elt fully included into existing */
                table[u].length += addedLength;
                table[u].savings += elt.savings * addedLength / elt.length; /* rough approx */
            }
            return ZDICT_rankMergedItem(table, u, table[u]);
        }

        if (MEM_read64(buf + table[u].pos) == MEM_read64(buf + elt.pos + 1)) {
            if (isIncluded(buf + table[u].pos, buf + elt.pos + 1, table[u].length)) {
                size_t const addedLength = MAX((int)elt.length - (int)table[u].length, 1);
                table[u].pos = elt.pos;
                table[u].savings += (U32)(elt.savings * addedLength / elt.length);
                table[u].length = MIN(elt.length, table[u].length + 1);
                return u;
            }
        }
    }

    return 0;
}
