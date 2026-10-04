/* Purpose: Rank sorted B-star groups with an explicit bounded cursor.
 * Inputs: Two live count-element regions of the suffix workspace, with sorter-produced signed group markers.
 * Outputs: Preserves the upstream group/rank representation; rejects malformed indices or an unterminated group. */
static int compute_typeBstar_ranks(int* sorted, int* ranks, int count) {
    int remaining = count;
    if (count < 0 || (count != 0 && (sorted == NULL || ranks == NULL)))
        return 0;
    while (remaining > 0) {
        const int groupEnd = remaining - 1;
        if (sorted[groupEnd] >= 0) {
            do {
                const int suffix = sorted[--remaining];
                if (suffix >= count)
                    return 0;
                ranks[suffix] = remaining;
            } while (remaining > 0 && sorted[remaining - 1] >= 0);
            sorted[remaining] = remaining - 1 - groupEnd;
            if (remaining <= 1)
                break;
        }
        {
            const int rank = remaining - 1;
            do {
                const int suffix = ~sorted[--remaining];
                if (suffix < 0 || suffix >= count)
                    return 0;
                sorted[remaining] = suffix;
                ranks[suffix] = rank;
            } while (remaining > 0 && sorted[remaining - 1] < 0);
            /* Sorter groups must terminate in a nonnegative member. */
            if (remaining == 0)
                return 0;
            --remaining;
            if (sorted[remaining] >= count)
                return 0;
            ranks[sorted[remaining]] = rank;
        }
    }
    return 1;
}

#ifdef SUPERZIP_ZSTD_SUFFIX_RANK_PROBES
/* Purpose: Exercise the exact production rank boundary with independent bounded fixtures.
 * Inputs: Live partial-sort/rank regions and their exact count. Outputs: The production success/error result. */
int sz_rank_partial_suffixes(int* sorted, int* ranks, int count) {
    return compute_typeBstar_ranks(sorted, ranks, count);
}
#endif
