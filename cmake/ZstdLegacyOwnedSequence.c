/* Purpose: Execute a legacy sequence with local prefix and independently owned history.
 * Inputs: Complete output/prefix and padded literal spans; history contains initialized bounded bytes.
 * Outputs: Exact sequence bytes or a native error before invalid geometry can form a pointer. */
static size_t ZSTDvXX_execSequence(BYTE* op, BYTE* const oend, seq_t sequence, const BYTE** litPtr,
                                   const BYTE* const litLimit, const BYTE* const base,
                                   const ZBUFF_ownedHistory* history) {
    size_t const available = op == NULL ? 0 : (size_t)(oend - op);
    size_t const literalAvailable = *litPtr == NULL ? 0 : (size_t)(litLimit - *litPtr);
    size_t sequenceLength;
    size_t prefixSize;
    size_t dictionaryOffset;
    BYTE* oLitEnd;
    BYTE* oMatchEnd;
    BYTE* oend_w;
    const BYTE* literalEnd;
    const BYTE* match;
    if (available < WILDCOPY_OVERLENGTH || sequence.litLength > available - WILDCOPY_OVERLENGTH ||
        sequence.matchLength > available - sequence.litLength)
        return ERROR(dstSize_tooSmall);
    if (*litPtr == NULL || base == NULL || sequence.litLength > literalAvailable || sequence.offset == 0)
        return ERROR(corruption_detected);
    sequenceLength = sequence.litLength + sequence.matchLength;
    oLitEnd = op + sequence.litLength;
    oMatchEnd = op + sequenceLength;
    oend_w = oend - WILDCOPY_OVERLENGTH;
    literalEnd = *litPtr + sequence.litLength;
    prefixSize = (size_t)(oLitEnd - base);
    dictionaryOffset = sequence.offset > prefixSize ? sequence.offset - prefixSize : 0;
    if (dictionaryOffset > ZBUFF_ownedHistorySize(history))
        return ERROR(corruption_detected);
    ZSTDvXX_wildcopy(op, *litPtr, (ptrdiff_t)sequence.litLength);
    op = oLitEnd;
    *litPtr = literalEnd;
    if (dictionaryOffset != 0) {
        size_t const copied = sequence.matchLength < dictionaryOffset ? sequence.matchLength : dictionaryOffset;
        if (ZBUFF_copyOwnedHistory(history, op, available - sequence.litLength, 0, dictionaryOffset, copied) !=
            ZBUFF_buffer_ok)
            return ERROR(corruption_detected);
        if (sequence.matchLength <= dictionaryOffset)
            return sequenceLength;
        op += dictionaryOffset;
        sequence.matchLength -= dictionaryOffset;
        match = base;
        if (op > oend_w || sequence.matchLength < MINMATCH) {
            while (op < oMatchEnd)
                *op++ = *match++;
            return sequenceLength;
        }
    } else {
        match = oLitEnd - sequence.offset;
    }
    if (sequence.offset < 8) {
        static const U32 dec32table[] = {0, 1, 2, 1, 4, 4, 4, 4};
        static const int dec64table[] = {8, 8, 8, 7, 8, 9, 10, 11};
        int const sub2 = dec64table[sequence.offset];
        op[0] = match[0];
        op[1] = match[1];
        op[2] = match[2];
        op[3] = match[3];
        match += dec32table[sequence.offset];
        ZSTDvXX_copy4(op + 4, match);
        match += 8 - sub2;
    } else {
        ZSTDvXX_copy8(op, match);
        match += 8;
    }
    op += 8;
    if ((size_t)(oend - oMatchEnd) < 16 - MINMATCH) {
        if (op < oend_w) {
            ZSTDvXX_wildcopy(op, match, oend_w - op);
            match += oend_w - op;
            op = oend_w;
        }
        while (op < oMatchEnd)
            *op++ = *match++;
    } else {
        ZSTDvXX_wildcopy(op, match, (ptrdiff_t)sequence.matchLength - 8);
    }
    return sequenceLength;
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Exercise the canonical sequence helper with actual independent history.
 * Inputs: Complete output and dictionary extents, initialized prefix and padded literal storage.
 * Outputs: Canonical result and matching owner cleanup, including allocation failure. */
size_t sz_legacy_vXX_sequence(void* destination, size_t capacity, size_t prefix, const void* literals,
                              size_t literalBytes, const void* dictionary, size_t dictionaryBytes, size_t literalLength,
                              size_t matchLength, size_t offset) {
    BYTE* output = (BYTE*)destination;
    const BYTE* cursor = (const BYTE*)literals;
    ZBUFF_bufferAllocator const allocator = {NULL, NULL, NULL};
    ZBUFF_ownedHistory* history;
    seq_t sequence;
    size_t result;
    if (prefix > capacity || capacity > PTRDIFF_MAX || (destination == NULL && capacity != 0))
        return ERROR(dstSize_tooSmall);
    if (literalBytes > PTRDIFF_MAX || (literals == NULL && literalBytes != 0) || dictionaryBytes > PTRDIFF_MAX ||
        (dictionary == NULL && dictionaryBytes != 0))
        return ERROR(corruption_detected);
    history = ZBUFF_createOwnedHistory(allocator);
    if (history == NULL)
        return ERROR(memory_allocation);
    if (ZBUFF_resetOwnedHistory(history, dictionaryBytes) != ZBUFF_buffer_ok ||
        ZBUFF_appendOwnedHistory(history, dictionary, dictionaryBytes, 0, dictionaryBytes) != ZBUFF_buffer_ok) {
        ZBUFF_releaseOwnedHistory(history);
        return ERROR(memory_allocation);
    }
    sequence.litLength = literalLength;
    sequence.matchLength = matchLength;
    sequence.offset = offset;
    result =
        ZSTDvXX_execSequence(prefix != 0 ? output + prefix : output, capacity != 0 ? output + capacity : output,
                             sequence, &cursor, literalBytes != 0 ? cursor + literalBytes : cursor, output, history);
    ZBUFF_releaseOwnedHistory(history);
    return result;
}
#endif
