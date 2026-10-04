typedef struct {
    U16 tableLog;
    U16 maxSymbolValue;
} FSE_CTableHeader;

typedef struct {
    size_t stateCount;
    size_t symbolCount;
    size_t transformOffset;
    size_t requiredBytes;
} FSE_CTableGeometry;

/* Purpose: Describe the packed table in byte and element units before any region pointer is formed.
 * Inputs: Declared table log and maximum byte symbol. Outputs: Valid nonoverlapping aligned regions or zeros.
 * The caller retains the existing FSE allocation contract for the returned requiredBytes. */
MEM_STATIC FSE_CTableGeometry FSE_getCTableGeometry(unsigned tableLog, unsigned maxSymbolValue) {
    FSE_CTableGeometry result = {0, 0, 0, 0};
    DEBUG_STATIC_ASSERT(sizeof(FSE_CTableHeader) == sizeof(FSE_CTable));
    DEBUG_STATIC_ASSERT(sizeof(FSE_symbolCompressionTransform) == 2 * sizeof(FSE_CTable));
    if (tableLog > 15 || maxSymbolValue > 255)
        return result;
    result.stateCount = tableLog == 0 ? 2 : (size_t)1 << tableLog;
    result.symbolCount = (size_t)maxSymbolValue + 1;
    result.transformOffset = sizeof(FSE_CTableHeader) + result.stateCount * sizeof(U16);
    result.requiredBytes = result.transformOffset + result.symbolCount * sizeof(FSE_symbolCompressionTransform);
    assert(result.transformOffset % sizeof(FSE_CTable) == 0);
    return result;
}

/* Purpose: Read the fixed packed header without a scaled U16 alias.
 * Inputs: A live FSE table meeting the existing allocation contract. Outputs: Header values by copy. */
MEM_STATIC FSE_CTableHeader FSE_readCTableHeader(const FSE_CTable* table) {
    FSE_CTableHeader result;
    const BYTE* const bytes = (const BYTE*)table;
    result.tableLog = MEM_read16(bytes + offsetof(FSE_CTableHeader, tableLog));
    result.maxSymbolValue = MEM_read16(bytes + offsetof(FSE_CTableHeader, maxSymbolValue));
    return result;
}
