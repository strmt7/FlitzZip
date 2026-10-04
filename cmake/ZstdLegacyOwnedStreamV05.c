/* Owned buffers and checked transfers for pinned Zstandard 1.5.7.
 * The enclosing source retains the upstream dual-license notice. */

/* Purpose: Measure one live byte cursor without null-pointer subtraction.
 * Inputs: begin/end delimit one borrowed live extent, or both are null.
 * Outputs: Returns distance, including zero for an empty null extent. */
static size_t ZBUFFv05_cursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Request complete scoped buffer replacements.
 * Inputs: Live exclusive decoder and frame-derived byte capacities.
 * Outputs: Version-specific ordinary error; failures preserve both original owners. */
static size_t ZBUFFv05_reserveBuffers(ZBUFFv05_DCtx* zbc, size_t inputSize, size_t outputSize) {
    ZBUFF_bufferResult const result = ZBUFF_reserveOwnedBuffers(zbc->buffers, inputSize, outputSize);
    if (result == ZBUFF_buffer_allocation_failure)
        return ERROR(memory_allocation);
    return result == ZBUFF_buffer_ok ? 0 : ERROR(corruption_detected);
}

/* Purpose: Accumulate a bounded legacy frame header.
 * Inputs: Decoder-owned header and live public input extent; load selects partial completion.
 * Outputs: Native hint/error and exact consumed bytes; invalid geometry is rejected before a copy. */
static size_t ZBUFFv05_loadFrameHeader(ZBUFFv05_DCtx* zbc, const char** ip, const char* iend, int load) {
    size_t available = ZBUFFv05_cursorDistance(iend, *ip);
    size_t headerSize;
    size_t count;
    if (zbc->hPos > sizeof(zbc->headerBuffer))
        return ERROR(corruption_detected);
    if (load) {
        count = MIN(sizeof(zbc->headerBuffer) - zbc->hPos, available);
        if (ZBUFF_copyBytes(zbc->headerBuffer, sizeof(zbc->headerBuffer), zbc->hPos, *ip, available, 0, count) !=
            ZBUFF_buffer_ok)
            return ERROR(corruption_detected);
        zbc->hPos += count;
        if (count != 0)
            *ip += count;
        headerSize = ZSTDv05_getFrameParams(&zbc->params, zbc->headerBuffer, zbc->hPos);
    } else {
        headerSize = ZSTDv05_getFrameParams(&zbc->params, *ip, available);
        if (ZSTDv05_isError(headerSize) || headerSize == 0)
            return headerSize;
        if (ZBUFF_copyBytes(zbc->headerBuffer, sizeof(zbc->headerBuffer), zbc->hPos, *ip, available, 0, available) !=
            ZBUFF_buffer_ok)
            return ERROR(corruption_detected);
        zbc->hPos += available;
    }
    if (ZSTDv05_isError(headerSize))
        return headerSize;
    if (headerSize != 0 && headerSize < zbc->hPos)
        return ERROR(corruption_detected);
    return headerSize != 0 ? headerSize - zbc->hPos : 0;
}

/* Purpose: Apply a validated header and acquire actual owned buffer geometry.
 * Inputs: Decoder-owned header and initialized version-specific frame parameters.
 * Outputs: Native error or the ready read/load stage, with complete allocation ownership. */
static size_t ZBUFFv05_prepareFrame(ZBUFFv05_DCtx* zbc) {
    size_t result;
    ZBUFF_bufferView buffers;
    if (zbc->params.windowLog >= sizeof(size_t) * 8)
        return ERROR(corruption_detected);
    result = ZBUFFv05_reserveBuffers(zbc, BLOCKSIZE, (size_t)1 << zbc->params.windowLog);
    if (ZSTDv05_isError(result))
        return result;
    buffers = ZBUFF_viewOwnedBuffers(zbc->buffers);
    if (zbc->hPos != 0) {
        if (ZBUFF_copyBytes(buffers.input, buffers.inputCapacity, 0, zbc->headerBuffer, sizeof(zbc->headerBuffer), 0,
                            zbc->hPos) != ZBUFF_buffer_ok)
            return ERROR(corruption_detected);
        zbc->inPos = zbc->hPos;
        zbc->hPos = 0;
        zbc->stage = ZBUFFv05ds_load;
        return 0;
    }
    zbc->stage = ZBUFFv05ds_read;
    return 0;
}

/* Purpose: Decode one complete input unit into the actual output allocation.
 * Inputs: Borrowed initialized input and validated owned output indices.
 * Outputs: Native error or published output interval and preserved stage semantics. */
static size_t ZBUFFv05_decodeInput(ZBUFFv05_DCtx* zbc, const char* source, size_t bytes, int buffered) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbc->buffers);
    size_t decodedSize;

    if (buffers.output == NULL || zbc->outStart > buffers.outputCapacity)
        return ERROR(corruption_detected);
    decodedSize = ZSTDv05_decompressContinue(zbc->zc, buffers.output + zbc->outStart,
                                             buffers.outputCapacity - zbc->outStart, source, bytes);
    if (ZSTDv05_isError(decodedSize))
        return decodedSize;
    if (decodedSize > buffers.outputCapacity - zbc->outStart)
        return ERROR(corruption_detected);
    if (buffered)
        zbc->inPos = 0;
    if (decodedSize == 0) {
        if (buffered)
            zbc->stage = ZBUFFv05ds_read;
        return 0;
    }
    zbc->outEnd = zbc->outStart + decodedSize;
    zbc->stage = ZBUFFv05ds_flush;
    return 0;
}

/* Purpose: Consume a complete public input unit or request buffered loading.
 * Inputs: Live input cursor and decoder state; notDone records backpressure.
 * Outputs: Native error or exact cursor/stage progress. */
static size_t ZBUFFv05_readInput(ZBUFFv05_DCtx* zbc, const char** ip, const char* iend, U32* notDone) {
    size_t const needed = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
    if (needed == 0) {
        zbc->stage = ZBUFFv05ds_init;
        *notDone = 0;
        return 0;
    }
    if (ZBUFFv05_cursorDistance(iend, *ip) >= needed) {
        size_t const result = ZBUFFv05_decodeInput(zbc, *ip, needed, 0);
        if (ZSTDv05_isError(result))
            return result;
        *ip += needed;
        return 0;
    }
    if (*ip == iend) {
        *notDone = 0;
        return 0;
    }
    zbc->stage = ZBUFFv05ds_load;
    return 0;
}

/* Purpose: Accumulate an input unit within actual owned input capacity.
 * Inputs: Live input cursor, initialized byte count and decoder need.
 * Outputs: Ordinary errors before writes or exact consumption and decode progress. */
static size_t ZBUFFv05_loadInput(ZBUFFv05_DCtx* zbc, const char** ip, const char* iend, U32* notDone) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbc->buffers);
    size_t const needed = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
    size_t const available = ZBUFFv05_cursorDistance(iend, *ip);
    size_t toLoad, loaded;
    if (zbc->inPos > needed || needed > buffers.inputCapacity)
        return ERROR(corruption_detected);
    toLoad = needed - zbc->inPos;
    loaded = MIN(toLoad, available);
    if (ZBUFF_copyBytes(buffers.input, buffers.inputCapacity, zbc->inPos, *ip, available, 0, loaded) != ZBUFF_buffer_ok)
        return ERROR(corruption_detected);
    if (loaded != 0)
        *ip += loaded;
    zbc->inPos += loaded;
    if (loaded < toLoad) {
        *notDone = 0;
        return 0;
    }
    return ZBUFFv05_decodeInput(zbc, buffers.input, needed, 1);
}

/* Purpose: Transfer only initialized output bytes and preserve wrap/backpressure.
 * Inputs: Live public output cursor and ordered owned output interval.
 * Outputs: Ordinary errors before transfer or exact flushed bytes and stage progress. */
static size_t ZBUFFv05_flushOutput(ZBUFFv05_DCtx* zbc, char** op, const char* oend, U32* notDone) {
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(zbc->buffers);
    size_t const available = ZBUFFv05_cursorDistance(oend, *op);
    size_t toFlush, flushed;
    if (zbc->outStart > zbc->outEnd || zbc->outEnd > buffers.outputCapacity)
        return ERROR(corruption_detected);
    toFlush = zbc->outEnd - zbc->outStart;
    flushed = MIN(available, toFlush);
    if (ZBUFF_copyBytes(*op, available, 0, buffers.output, buffers.outputCapacity, zbc->outStart, flushed) !=
        ZBUFF_buffer_ok)
        return ERROR(corruption_detected);
    if (flushed != 0)
        *op += flushed;
    zbc->outStart += flushed;
    if (flushed == toFlush) {
        zbc->stage = ZBUFFv05ds_read;
        if (BLOCKSIZE > buffers.outputCapacity - zbc->outStart)
            zbc->outStart = zbc->outEnd = 0;
    } else {
        *notDone = 0;
    }
    return 0;
}

/* Purpose: Advance an initialized legacy stream through checked stages.
 * Inputs: Caller-owned live buffers/counters; decoder exclusively owns its buffer tree.
 * Outputs: Native errors, hints and consumption; invalid public geometry is rejected before pointer formation. */
size_t ZBUFFv05_decompressContinue(ZBUFFv05_DCtx* zbc, void* dst, size_t* maxDstSizePtr, const void* src,
                                   size_t* srcSizePtr) {
    const char *istart, *iend, *ip;
    char *ostart, *op;
    const char* oend;
    U32 notDone = 1;
    if (zbc == NULL || zbc->buffers == NULL || srcSizePtr == NULL || maxDstSizePtr == NULL)
        return ERROR(GENERIC);
    if (*srcSizePtr > (size_t)PTRDIFF_MAX || *maxDstSizePtr > (size_t)PTRDIFF_MAX ||
        (src == NULL && *srcSizePtr != 0) || (dst == NULL && *maxDstSizePtr != 0))
        return ERROR(GENERIC);
    istart = ip = (const char*)src;
    iend = *srcSizePtr != 0 ? istart + *srcSizePtr : istart;
    ostart = op = (char*)dst;
    oend = *maxDstSizePtr != 0 ? ostart + *maxDstSizePtr : ostart;
    while (notDone) {
        switch (zbc->stage) {
        case ZBUFFv05ds_init:
            return ERROR(init_missing);
        case ZBUFFv05ds_readHeader:
        case ZBUFFv05ds_loadHeader: {
            int const load = zbc->stage == ZBUFFv05ds_loadHeader;
            size_t const result = ZBUFFv05_loadFrameHeader(zbc, &ip, iend, load);
            if (ZSTDv05_isError(result))
                return result;
            if (result != 0) {
                *maxDstSizePtr = 0;
                zbc->stage = ZBUFFv05ds_loadHeader;
                return result;
            }
            zbc->stage = ZBUFFv05ds_decodeHeader;
        }
        /* Complete header processing continues immediately. */
        case ZBUFFv05ds_decodeHeader: {
            size_t const result = ZBUFFv05_prepareFrame(zbc);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_read)
                break;
        }
        /* Ready frames continue immediately into reading. */
        case ZBUFFv05ds_read: {
            size_t const result = ZBUFFv05_readInput(zbc, &ip, iend, &notDone);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_load)
                break;
        }
        /* Partial input continues immediately into loading. */
        case ZBUFFv05ds_load: {
            size_t const result = ZBUFFv05_loadInput(zbc, &ip, iend, &notDone);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_flush)
                break;
        }
        /* Decoded output is flushed in the same iteration. */
        case ZBUFFv05ds_flush: {
            size_t const result = ZBUFFv05_flushOutput(zbc, &op, oend, &notDone);
            if (ZSTDv05_isError(result))
                return result;
            break;
        }
        default:
            return ERROR(GENERIC);
        }
    }
    *srcSizePtr = ZBUFFv05_cursorDistance(ip, istart);
    *maxDstSizePtr = ZBUFFv05_cursorDistance(op, ostart);
    {
        size_t hint = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
        if (hint > ZBUFFv05_blockHeaderSize)
            hint += ZBUFFv05_blockHeaderSize;
        if (hint < zbc->inPos)
            return ERROR(corruption_detected);
        return hint - zbc->inPos;
    }
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Inspect actual owned buffer geometry without accessing absent storage.
 * Inputs: One live version-specific buffered decoder. Outputs: True only for coherent allocated pairs. */
int sz_legacy_v05_buffers_consistent(const void* context) {
    const ZBUFFv05_DCtx* decoder = (const ZBUFFv05_DCtx*)context;
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(decoder->buffers);
    return decoder->buffers != NULL && (buffers.inputCapacity == 0 || buffers.input != NULL) &&
           (buffers.outputCapacity == 0 || buffers.output != NULL);
}

/* Purpose: Borrow allocation identities without accessing their contents.
 * Inputs: One live decoder. Outputs: Actual owned identities/capacities; no ownership transfer. */
sz_legacy_buffer_state sz_legacy_v05_get_buffer_state(const void* context) {
    const ZBUFFv05_DCtx* decoder = (const ZBUFFv05_DCtx*)context;
    ZBUFF_bufferView const buffers = ZBUFF_viewOwnedBuffers(decoder->buffers);
    sz_legacy_buffer_state result = {buffers.input, buffers.inputCapacity, buffers.output, buffers.outputCapacity};
    return result;
}
#endif
