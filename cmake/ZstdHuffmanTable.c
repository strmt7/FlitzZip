typedef struct {
    HUF_DEltX1* entries;
    size_t capacity;
} HUF_X1TableView;

/* Purpose: Fill a Huffman table in entry units after validating the complete ranked representation.
 * Inputs: Live typed entry storage, ordered symbols, rank counts through tableLog, and a valid table log.
 * Outputs: Returns zero or corruption_detected; malformed geometry leaves every entry unchanged. */
static size_t HUF_fillX1Table(HUF_X1TableView table, const BYTE* symbols, size_t symbolCount, const U32* ranks,
                              U32 tableLog) {
    size_t symbol, entries = 0;
    U32 weight;
    if (table.entries == NULL || symbols == NULL || ranks == NULL || tableLog > HUF_TABLELOG_MAX)
        return ERROR(corruption_detected);
    symbol = ranks[0];
    if (symbol > symbolCount)
        return ERROR(corruption_detected);
    /* Validate all ranges before publishing any table entry. */
    for (weight = 1; weight <= tableLog; ++weight) {
        size_t const count = ranks[weight];
        size_t const length = (size_t)1 << (weight - 1);
        if (count > symbolCount - symbol || entries > table.capacity || count > (table.capacity - entries) / length)
            return ERROR(corruption_detected);
        symbol += count;
        entries += count * length;
    }
    if (symbol != symbolCount || entries != (size_t)1 << tableLog)
        return ERROR(corruption_detected);
    symbol = ranks[0];
    entries = 0;
    for (weight = 1; weight <= tableLog; ++weight) {
        size_t const length = (size_t)1 << (weight - 1);
        size_t ranked;
        for (ranked = 0; ranked < ranks[weight]; ++ranked) {
            HUF_DEltX1 const value = {(BYTE)(tableLog + 1 - weight), symbols[symbol++]};
            size_t repeat;
            for (repeat = 0; repeat < length; ++repeat)
                table.entries[entries++] = value;
        }
    }
    return 0;
}
