/* Owned buffers and checked transfers for pinned Zstandard 1.5.7.
 * The enclosing source retains the upstream dual-license notice. */

/* Purpose: Measure one live byte cursor without null-pointer subtraction.
 * Inputs: begin/end delimit one borrowed live extent, or both are null.
 * Outputs: Returns distance, including zero for an empty null extent. */
static size_t ZBUFFv06_cursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Request complete scoped buffer replacements.
 * Inputs: Live exclusive decoder and frame-derived byte capacities.
 * Outputs: Version-specific ordinary error; failures preserve both original owners. */
static size_t ZBUFFv06_reserveBuffers(ZBUFFv06_DCtx* zbd, size_t inputSize, size_t outputSize) {
    ZBUFF_bufferResult const result = ZBUFF_reserveOwnedBuffers(zbd->buffers, inputSize, outputSize);
    if (result == ZBUFF_buffer_allocation_failure)
        return ERROR(memory_allocation);
    return result == ZBUFF_buffer_ok ? 0 : ERROR(corruption_detected);
}

/* Purpose: Accumulate a bounded legacy frame header.
 * Inputs: Decoder-owned header and live public input extent.
 * Outputs: Native remaining-byte hint/error and exact consumption; complete input advances without stalling. */
static size_t ZBUFFv06_loadFrameHeader(ZBUFFv06_DCtx* zbd, const char** ip, const char* iend) {
    for (;;) {
        size_t const available = ZBUFFv06_cursorDistance(iend, *ip);
        size_t headerSize, count;
        if (zbd->lhSize > sizeof(zbd->headerBuffer))
            return ERROR(corruption_detected);
        headerSize = ZSTDv06_getFrameParams(&zbd->fParams, zbd->headerBuffer, zbd->lhSize);
        if (ZSTDv06_isError(headerSize) || headerSize == 0)
            return headerSize;
        if (headerSize <= zbd->lhSize || headerSize > sizeof(zbd->headerBuffer))
            return ERROR(corruption_detected);
        count = MIN(headerSize - zbd->lhSize, available);
        if (ZBUFF_copyBytes(zbd->headerBuffer, sizeof(zbd->headerBuffer), zbd->lhSize, *ip, available, 0, count) !=
            ZBUFF_buffer_ok)
            return ERROR(corruption_detected);
        zbd->lhSize += count;
        if (count != 0)
            *ip += count;
        if (headerSize != zbd->lhSize)
            return headerSize - zbd->lhSize;
        /* A completed prefix can reveal a longer header; consume it in this call. */
    }
}

/* Purpose: Apply a validated header and acquire actual owned buffer geometry.
 * Inputs: Decoder-owned header and initialized version-specific frame parameters.
 * Outputs: Native error or the ready read/load stage, with complete allocation ownership. */
static size_t ZBUFFv06_prepareFrame(ZBUFFv06_DCtx* zbd) {
    size_t const h1Size = ZSTDv06_nextSrcSizeToDecompress(zbd->zd);
    size_t result;
    size_t windowSize, blockSize, outputSize;
    if (zbd->lhSize > sizeof(zbd->headerBuffer) || h1Size > zbd->lhSize)
        return ERROR(corruption_detected);
    result = ZSTDv06_decompressContinue(zbd->zd, NULL, 0, zbd->headerBuffer, h1Size);
    if (ZSTDv06_isError(result))
        return result;
    if (h1Size < zbd->lhSize) {
        size_t const h2Size = ZSTDv06_nextSrcSizeToDecompress(zbd->zd);
        if (h2Size > zbd->lhSize - h1Size)
            return ERROR(corruption_detected);
        result = ZSTDv06_decompressContinue(zbd->zd, NULL, 0, zbd->headerBuffer + h1Size, h2Size);
        if (ZSTDv06_isError(result))
            return result;
    }
    if (zbd->fParams.windowLog >= sizeof(size_t) * 8)
        return ERROR(corruption_detected);
    windowSize = (size_t)1 << zbd->fParams.windowLog;
    blockSize = MIN(windowSize, ZSTDv06_BLOCKSIZE_MAX);
    if (windowSize > (size_t)PTRDIFF_MAX - blockSize - WILDCOPY_OVERLENGTH * 2)
        return ERROR(corruption_detected);
    outputSize = windowSize + blockSize + WILDCOPY_OVERLENGTH * 2;
    result = ZBUFFv06_reserveBuffers(zbd, blockSize, outputSize);
    if (ZSTDv06_isError(result))
        return result;
    zbd->blockSize = blockSize;
    zbd->stage = ZBUFFds_read;
    return 0;
}

/* Purpose: Decode one complete input unit into the actual output allocation.
 * Inputs: Borrowed initialized input and validated owned output indices.
 * Outputs: Native error or published output interval and preserved stage semantics. */
static size_t ZBUFFv06_decodeInput(ZBUFFv06_DCtx* zbd, const char* source, size_t bytes, int buffered) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbd->buffers);
    size_t decodedSize;

    if (buffers.output == NULL || zbd->outStart > buffers.outputCapacity)
        return ERROR(corruption_detected);
    decodedSize = ZSTDv06_decompressContinue(zbd->zd, buffers.output + zbd->outStart,
                                             buffers.outputCapacity - zbd->outStart, source, bytes);
    if (ZSTDv06_isError(decodedSize))
        return decodedSize;
    if (decodedSize > buffers.outputCapacity - zbd->outStart)
        return ERROR(corruption_detected);
    if (buffered)
        zbd->inPos = 0;
    if (decodedSize == 0) {
        if (buffered)
            zbd->stage = ZBUFFds_read;
        return 0;
    }
    zbd->outEnd = zbd->outStart + decodedSize;
    zbd->stage = ZBUFFds_flush;
    return 0;
}

/* Purpose: Consume a complete public input unit or request buffered loading.
 * Inputs: Live input cursor and decoder state; notDone records backpressure.
 * Outputs: Native error or exact cursor/stage progress. */
static size_t ZBUFFv06_readInput(ZBUFFv06_DCtx* zbd, const char** ip, const char* iend, U32* notDone) {
    size_t const needed = ZSTDv06_nextSrcSizeToDecompress(zbd->zd);
    if (needed == 0) {
        zbd->stage = ZBUFFds_init;
        *notDone = 0;
        return 0;
    }
    if (ZBUFFv06_cursorDistance(iend, *ip) >= needed) {
        size_t const result = ZBUFFv06_decodeInput(zbd, *ip, needed, 0);
        if (ZSTDv06_isError(result))
            return result;
        *ip += needed;
        return 0;
    }
    if (*ip == iend) {
        *notDone = 0;
        return 0;
    }
    zbd->stage = ZBUFFds_load;
    return 0;
}

/* Purpose: Accumulate an input unit within actual owned input capacity.
 * Inputs: Live input cursor, initialized byte count and decoder need.
 * Outputs: Ordinary errors before writes or exact consumption and decode progress. */
static size_t ZBUFFv06_loadInput(ZBUFFv06_DCtx* zbd, const char** ip, const char* iend, U32* notDone) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbd->buffers);
    size_t const needed = ZSTDv06_nextSrcSizeToDecompress(zbd->zd);
    size_t const available = ZBUFFv06_cursorDistance(iend, *ip);
    size_t toLoad, loaded;
    if (zbd->inPos > needed || needed > buffers.inputCapacity)
        return ERROR(corruption_detected);
    toLoad = needed - zbd->inPos;
    loaded = MIN(toLoad, available);
    if (ZBUFF_copyBytes(buffers.input, buffers.inputCapacity, zbd->inPos, *ip, available, 0, loaded) != ZBUFF_buffer_ok)
        return ERROR(corruption_detected);
    if (loaded != 0)
        *ip += loaded;
    zbd->inPos += loaded;
    if (loaded < toLoad) {
        *notDone = 0;
        return 0;
    }
    return ZBUFFv06_decodeInput(zbd, buffers.input, needed, 1);
}

/* Purpose: Transfer only initialized output bytes and preserve wrap/backpressure.
 * Inputs: Live public output cursor and ordered owned output interval.
 * Outputs: Ordinary errors before transfer or exact flushed bytes and stage progress. */
static size_t ZBUFFv06_flushOutput(ZBUFFv06_DCtx* zbd, char** op, const char* oend, U32* notDone) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbd->buffers);
    size_t const available = ZBUFFv06_cursorDistance(oend, *op);
    size_t toFlush, flushed;
    if (zbd->outStart > zbd->outEnd || zbd->outEnd > buffers.outputCapacity)
        return ERROR(corruption_detected);
    toFlush = zbd->outEnd - zbd->outStart;
    flushed = MIN(available, toFlush);
    if (ZBUFF_copyBytes(*op, available, 0, buffers.output, buffers.outputCapacity, zbd->outStart, flushed) !=
        ZBUFF_buffer_ok)
        return ERROR(corruption_detected);
    if (flushed != 0)
        *op += flushed;
    zbd->outStart += flushed;
    if (flushed == toFlush) {
        zbd->stage = ZBUFFds_read;
        if (zbd->blockSize > buffers.outputCapacity - zbd->outStart)
            zbd->outStart = zbd->outEnd = 0;
    } else {
        *notDone = 0;
    }
    return 0;
}

/* Purpose: Advance an initialized legacy stream through checked stages.
 * Inputs: Caller-owned live buffers/counters; decoder exclusively owns its buffer tree.
 * Outputs: Native errors, hints and consumption; invalid public geometry is rejected before pointer formation. */
size_t ZBUFFv06_decompressContinue(ZBUFFv06_DCtx* zbd, void* dst, size_t* dstCapacityPtr, const void* src,
                                   size_t* srcSizePtr) {
    const char *istart, *iend, *ip;
    char *ostart, *op;
    const char* oend;
    U32 notDone = 1;
    if (zbd == NULL || zbd->buffers == NULL || srcSizePtr == NULL || dstCapacityPtr == NULL)
        return ERROR(GENERIC);
    if (*srcSizePtr > (size_t)PTRDIFF_MAX || *dstCapacityPtr > (size_t)PTRDIFF_MAX ||
        (src == NULL && *srcSizePtr != 0) || (dst == NULL && *dstCapacityPtr != 0))
        return ERROR(GENERIC);
    istart = ip = (const char*)src;
    iend = *srcSizePtr != 0 ? istart + *srcSizePtr : istart;
    ostart = op = (char*)dst;
    oend = *dstCapacityPtr != 0 ? ostart + *dstCapacityPtr : ostart;
    while (notDone) {
        switch (zbd->stage) {
        case ZBUFFds_init:
            return ERROR(init_missing);
        case ZBUFFds_loadHeader: {
            size_t const result = ZBUFFv06_loadFrameHeader(zbd, &ip, iend);
            if (ZSTDv06_isError(result))
                return result;
            if (result != 0) {
                *dstCapacityPtr = 0;
                return result + ZSTDv06_blockHeaderSize;
            }
        }
            {
                size_t const result = ZBUFFv06_prepareFrame(zbd);
                if (ZSTDv06_isError(result))
                    return result;
            }
        /* Ready frames continue immediately into reading. */
        case ZBUFFds_read: {
            size_t const result = ZBUFFv06_readInput(zbd, &ip, iend, &notDone);
            if (ZSTDv06_isError(result))
                return result;
            if (zbd->stage != ZBUFFds_load)
                break;
        }
        /* Partial input continues immediately into loading. */
        case ZBUFFds_load: {
            size_t const result = ZBUFFv06_loadInput(zbd, &ip, iend, &notDone);
            if (ZSTDv06_isError(result))
                return result;
            if (zbd->stage != ZBUFFds_flush)
                break;
        }
        /* Decoded output is flushed in the same iteration. */
        case ZBUFFds_flush: {
            size_t const result = ZBUFFv06_flushOutput(zbd, &op, oend, &notDone);
            if (ZSTDv06_isError(result))
                return result;
            break;
        }
        default:
            return ERROR(GENERIC);
        }
    }
    *srcSizePtr = ZBUFFv06_cursorDistance(ip, istart);
    *dstCapacityPtr = ZBUFFv06_cursorDistance(op, ostart);
    {
        size_t hint = ZSTDv06_nextSrcSizeToDecompress(zbd->zd);
        if (hint > ZSTDv06_blockHeaderSize)
            hint += ZSTDv06_blockHeaderSize;
        if (hint < zbd->inPos)
            return ERROR(corruption_detected);
        return hint - zbd->inPos;
    }
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Inspect actual owned buffer geometry without accessing absent storage.
 * Inputs: One live version-specific buffered decoder. Outputs: True only for coherent allocated pairs. */
int sz_legacy_v06_buffers_consistent(const void* context) {
    const ZBUFFv06_DCtx* decoder = (const ZBUFFv06_DCtx*)context;
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(decoder->buffers);
    return decoder->buffers != NULL && (buffers.inputCapacity == 0 || buffers.input != NULL) &&
           (buffers.outputCapacity == 0 || buffers.output != NULL);
}

/* Purpose: Borrow allocation identities without accessing their contents.
 * Inputs: One live decoder. Outputs: Actual owned identities/capacities; no ownership transfer. */
sz_legacy_buffer_state sz_legacy_v06_get_buffer_state(const void* context) {
    const ZBUFFv06_DCtx* decoder = (const ZBUFFv06_DCtx*)context;
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(decoder->buffers);
    sz_legacy_buffer_state result = {buffers.input, buffers.inputCapacity, buffers.output, buffers.outputCapacity};
    return result;
}
#endif
