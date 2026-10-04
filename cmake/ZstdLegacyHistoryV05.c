/* Purpose: Execute a legacy sequence after proving output, literal and history extents.
 * Inputs: Output/prefix and padded literals are live borrowed spans; dictEnd ends dictSize history bytes.
 * Outputs: Returns bytes produced or an error before invalid pointer formation or access. */
static size_t ZSTDv05_execSequence(BYTE* op, BYTE* const oend, seq_t sequence, const BYTE** litPtr,
                                   const BYTE* const litLimit, const BYTE* const base, size_t dictSize,
                                   const BYTE* const dictEnd) {
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
    if (dictionaryOffset > dictSize)
        return ERROR(corruption_detected);
    ZSTDv05_wildcopy(op, *litPtr, (ptrdiff_t)sequence.litLength);
    op = oLitEnd;
    *litPtr = literalEnd;
    if (dictionaryOffset != 0) {
        match = dictEnd - dictionaryOffset;
        if (sequence.matchLength <= dictionaryOffset) {
            memmove(op, match, sequence.matchLength);
            return sequenceLength;
        }
        memmove(op, match, dictionaryOffset);
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
        ZSTDv05_copy4(op + 4, match);
        match += 8 - sub2;
    } else {
        ZSTDv05_copy8(op, match);
        match += 8;
    }
    op += 8;
    if ((size_t)(oend - oMatchEnd) < 16 - MINMATCH) {
        if (op < oend_w) {
            ZSTDv05_wildcopy(op, match, oend_w - op);
            match += oend_w - op;
            op = oend_w;
        }
        while (op < oMatchEnd)
            *op++ = *match++;
    } else {
        ZSTDv05_wildcopy(op, match, (ptrdiff_t)sequence.matchLength - 8);
    }
    return sequenceLength;
}

/* Purpose: Measure the current borrowed prefix without subtracting null pointers.
 * Inputs: dctx retains one live prefix or an empty initialized state.
 * Outputs: Returns only its initialized byte count, never a fabricated virtual address. */
static size_t ZSTDv05_historySize(const ZSTDv05_DCtx* dctx) {
    return dctx->base == NULL ? 0 : (size_t)((const BYTE*)dctx->previousDstEnd - (const BYTE*)dctx->base);
}

/* Purpose: Preserve a previous prefix as bounded dictionary history on a discontinuous destination.
 * Inputs: dst and the prior prefix remain live under the legacy streaming API's borrowed-storage contract.
 * Outputs: Records dictionary byte extent and starts the new prefix without forming an out-of-object pointer. */
static void ZSTDv05_checkContinuity(ZSTDv05_DCtx* dctx, const void* dst) {
    if (dst != dctx->previousDstEnd) {
        dctx->dictSize = ZSTDv05_historySize(dctx);
        dctx->dictEnd = dctx->previousDstEnd;
        dctx->base = dst;
        dctx->previousDstEnd = dst;
    }
}

/* Purpose: Borrow initialized dictionary content using an explicit prefix length.
 * Inputs: dict owns dictSize readable bytes until decoder use ends; old borrowed storage remains live.
 * Outputs: Publishes the new prefix and bounded prior dictionary history without null or virtual arithmetic. */
static void ZSTDv05_refDictContent(ZSTDv05_DCtx* dctx, const void* dict, size_t dictSize) {
    dctx->dictSize = ZSTDv05_historySize(dctx);
    dctx->dictEnd = dctx->previousDstEnd;
    dctx->base = dict;
    dctx->previousDstEnd = dictSize != 0 ? (const BYTE*)dict + dictSize : dict;
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Exercise the complete canonical sequence helper with explicit test extents.
 * Inputs: destination owns capacity bytes, prefix <= capacity, and literals have upstream wildcopy padding.
 * Outputs: Returns the unchanged helper result; has no alternate decoder or product-facing API. */
size_t sz_legacy_v05_sequence(void* destination, size_t capacity, size_t prefix, const void* literals,
                              size_t literalBytes, const void* dictionary, size_t dictionaryBytes, size_t literalLength,
                              size_t matchLength, size_t offset) {
    BYTE* output = (BYTE*)destination;
    const BYTE* cursor = (const BYTE*)literals;
    seq_t sequence;
    if (prefix > capacity)
        return ERROR(dstSize_tooSmall);
    sequence.litLength = literalLength;
    sequence.matchLength = matchLength;
    sequence.offset = offset;
    return ZSTDv05_execSequence(
        prefix != 0 ? output + prefix : output, capacity != 0 ? output + capacity : output, sequence, &cursor,
        literalBytes != 0 ? cursor + literalBytes : cursor, output, dictionaryBytes,
        dictionaryBytes != 0 ? (const BYTE*)dictionary + dictionaryBytes : (const BYTE*)dictionary);
}
#endif
