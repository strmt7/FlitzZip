/* Transactional buffers and explicit stream stages for pinned Zstandard 1.5.7.
 * The enclosing source retains the upstream dual-license notice. */

/* Purpose: Measure a public legacy byte cursor without null-pointer subtraction.
 * Inputs: begin/end delimit one borrowed live extent, or are both null when empty.
 * Outputs: Returns remaining or consumed bytes; empty null extents have distance zero. */
static size_t ZBUFFv07_cursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Acquire stream buffers with a strong ownership guarantee.
 * Inputs: The live decoder owns current buffers; requested capacities come from validated frame parameters.
 * Outputs: On error retains both original owners/capacities; on success publishes both complete replacements. */
static size_t ZBUFFv07_reserveBuffers(ZBUFFv07_DCtx* zbd, size_t inputSize, size_t outputSize) {
    char* replacementIn = NULL;
    char* replacementOut = NULL;
    /* Acquire every required replacement before publishing or releasing owners. */
    if (zbd->inBuffSize < inputSize) {
        replacementIn = (char*)zbd->customMem.customAlloc(zbd->customMem.opaque, inputSize);
        if (replacementIn == NULL)
            return ERROR(memory_allocation);
    }
    if (zbd->outBuffSize < outputSize) {
        replacementOut = (char*)zbd->customMem.customAlloc(zbd->customMem.opaque, outputSize);
        if (replacementOut == NULL) {
            zbd->customMem.customFree(zbd->customMem.opaque, replacementIn);
            return ERROR(memory_allocation);
        }
    }
    /* Commit complete capacity/storage pairs only after both acquisitions succeed. */
    if (replacementIn != NULL) {
        zbd->customMem.customFree(zbd->customMem.opaque, zbd->inBuff);
        zbd->inBuff = replacementIn;
        zbd->inBuffSize = inputSize;
    }
    if (replacementOut != NULL) {
        zbd->customMem.customFree(zbd->customMem.opaque, zbd->outBuff);
        zbd->outBuff = replacementOut;
        zbd->outBuffSize = outputSize;
    }
    return 0;
}

/* Purpose: Apply a complete validated frame header and prepare owned stream storage.
 * Inputs: The decoder owns its accumulated header and allocator; this call borrows no input.
 * Outputs: Returns an error or publishes the read/load stage with valid buffer ownership. */
static size_t ZBUFFv07_prepareFrame(ZBUFFv07_DCtx* zbd) {
    /* Consume header */
    {
        size_t const h1Size = ZSTDv07_nextSrcSizeToDecompress(zbd->zd); /* == ZSTDv07_frameHeaderSize_min */
        size_t const h1Result = ZSTDv07_decompressContinue(zbd->zd, NULL, 0, zbd->headerBuffer, h1Size);
        if (ZSTDv07_isError(h1Result))
            return h1Result;
        if (h1Size < zbd->lhSize) { /* long header */
            size_t const h2Size = ZSTDv07_nextSrcSizeToDecompress(zbd->zd);
            size_t const h2Result = ZSTDv07_decompressContinue(zbd->zd, NULL, 0, zbd->headerBuffer + h1Size, h2Size);
            if (ZSTDv07_isError(h2Result))
                return h2Result;
        }
    }

    zbd->fParams.windowSize = MAX(zbd->fParams.windowSize, 1U << ZSTDv07_WINDOWLOG_ABSOLUTEMIN);
    {
        size_t const windowSize = (size_t)zbd->fParams.windowSize;
        size_t const blockSize = MIN(windowSize, ZSTDv07_BLOCKSIZE_ABSOLUTEMAX);
        size_t const outputSize = windowSize + blockSize + WILDCOPY_OVERLENGTH * 2;
        size_t const result = ZBUFFv07_reserveBuffers(zbd, blockSize, outputSize);
        if (ZSTDv07_isError(result))
            return result;
        zbd->blockSize = blockSize;
    }
    zbd->stage = ZBUFFds_read;
    return 0;
}

/* Purpose: Process the buffered stream read stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static size_t ZBUFFv07_readInput(ZBUFFv07_DCtx* zbd, const char** ip, const char* iend, U32* notDone) {
    size_t const neededInSize = ZSTDv07_nextSrcSizeToDecompress(zbd->zd);
    if (neededInSize == 0) { /* end of frame */
        zbd->stage = ZBUFFds_init;
        (*notDone) = 0;
        return 0;
    }
    if ((size_t)(ZBUFFv07_cursorDistance(iend, *ip)) >= neededInSize) { /* decode directly from src */
        const int isSkipFrame = ZSTDv07_isSkipFrame(zbd->zd);
        size_t const decodedSize =
            ZSTDv07_decompressContinue(zbd->zd, zbd->outBuff + zbd->outStart,
                                       (isSkipFrame ? 0 : zbd->outBuffSize - zbd->outStart), (*ip), neededInSize);
        if (ZSTDv07_isError(decodedSize))
            return decodedSize;
        (*ip) += neededInSize;
        if (!decodedSize && !isSkipFrame)
            return 0; /* this was just a header */
        zbd->outEnd = zbd->outStart + decodedSize;
        zbd->stage = ZBUFFds_flush;
        return 0;
    }
    if ((*ip) == iend) {
        (*notDone) = 0;
        return 0;
    } /* no more input */
    zbd->stage = ZBUFFds_load;
    return 0;
}

/* Purpose: Process the buffered stream load stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static size_t ZBUFFv07_loadInput(ZBUFFv07_DCtx* zbd, const char** ip, const char* iend, U32* notDone) {
    size_t const neededInSize = ZSTDv07_nextSrcSizeToDecompress(zbd->zd);
    size_t const toLoad = neededInSize - zbd->inPos; /* should always be <= remaining space within inBuff */
    size_t loadedSize;
    if (toLoad > zbd->inBuffSize - zbd->inPos)
        return ERROR(corruption_detected); /* should never happen */
    loadedSize = ZBUFFv07_limitCopy(zbd->inBuff + zbd->inPos, toLoad, (*ip), ZBUFFv07_cursorDistance(iend, *ip));
    if (loadedSize != 0)
        (*ip) += loadedSize;
    zbd->inPos += loadedSize;
    if (loadedSize < toLoad) {
        (*notDone) = 0;
        return 0;
    } /* not enough input, wait for more */

    /* decode loaded input */
    {
        const int isSkipFrame = ZSTDv07_isSkipFrame(zbd->zd);
        size_t const decodedSize = ZSTDv07_decompressContinue(
            zbd->zd, zbd->outBuff + zbd->outStart, zbd->outBuffSize - zbd->outStart, zbd->inBuff, neededInSize);
        if (ZSTDv07_isError(decodedSize))
            return decodedSize;
        zbd->inPos = 0; /* input is consumed */
        if (!decodedSize && !isSkipFrame) {
            zbd->stage = ZBUFFds_read;
            return 0;
        } /* this was just a header */
        zbd->outEnd = zbd->outStart + decodedSize;
        zbd->stage = ZBUFFds_flush;
        /* Fall through to the flush stage. */
        /* pass-through */
    }
    return 0;
}

/* Purpose: Process the buffered stream flush stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static void ZBUFFv07_flushOutput(ZBUFFv07_DCtx* zbd, char** op, const char* oend, U32* notDone) {
    size_t const toFlushSize = zbd->outEnd - zbd->outStart;
    size_t const flushedSize = ZBUFFv07_limitCopy((*op), ZBUFFv07_cursorDistance(oend, *op), zbd->outBuff + zbd->outStart, toFlushSize);
    if (flushedSize != 0)
        (*op) += flushedSize;
    zbd->outStart += flushedSize;
    if (flushedSize == toFlushSize) {
        zbd->stage = ZBUFFds_read;
        if (zbd->outStart + zbd->blockSize > zbd->outBuffSize)
            zbd->outStart = zbd->outEnd = 0;
        return;
    }
    /* cannot flush everything */
    (*notDone) = 0;
    return;
}

/* Purpose: Advance an initialized legacy stream through small explicit stages.
 * Inputs: The caller owns source/output and capacity counters; decoder exclusively owns its history buffers.
 * Outputs: Preserves native consumption and hints; failed acquisition retains valid owned storage. */
size_t ZBUFFv07_decompressContinue(ZBUFFv07_DCtx* zbd, void* dst, size_t* dstCapacityPtr, const void* src,
                                   size_t* srcSizePtr) {
    const char* const istart = (const char*)src;
    const char* const iend = *srcSizePtr != 0 ? istart + *srcSizePtr : istart;
    const char* ip = istart;
    char* const ostart = (char*)dst;
    char* const oend = *dstCapacityPtr != 0 ? ostart + *dstCapacityPtr : ostart;
    char* op = ostart;
    U32 notDone = 1;

    while (notDone) {
        switch (zbd->stage) {
        case ZBUFFds_init:
            return ERROR(init_missing);
        case ZBUFFds_loadHeader: {
            size_t const hSize = ZSTDv07_getFrameParams(&(zbd->fParams), zbd->headerBuffer, zbd->lhSize);
            if (ZSTDv07_isError(hSize))
                return hSize;
            if (hSize != 0) {
                size_t const toLoad = hSize - zbd->lhSize; /* if hSize!=0, hSize > zbd->lhSize */
                if (toLoad > (size_t)(ZBUFFv07_cursorDistance(iend, ip))) {        /* not enough input to load full header */
                    if (ip != NULL)
                        memcpy(zbd->headerBuffer + zbd->lhSize, ip, ZBUFFv07_cursorDistance(iend, ip));
                    zbd->lhSize += ZBUFFv07_cursorDistance(iend, ip);
                    *dstCapacityPtr = 0;
                    return (hSize - zbd->lhSize) +
                           ZSTDv07_blockHeaderSize; /* remaining header bytes + next block header */
                }
                memcpy(zbd->headerBuffer + zbd->lhSize, ip, toLoad);
                zbd->lhSize = hSize;
                ip += toLoad;
                break;
            }
        }

            {
                size_t const result = ZBUFFv07_prepareFrame(zbd);
                if (ZSTDv07_isError(result))
                    return result;
                if (zbd->stage != ZBUFFds_read)
                    break;
            }
            /* A complete frame preparation immediately enters the read stage. */
        case ZBUFFds_read: {
            size_t const result = ZBUFFv07_readInput(zbd, &ip, iend, &notDone);
            if (ZSTDv07_isError(result))
                return result;
            if (zbd->stage != ZBUFFds_load)
                break;
        }
            /* A partial direct read needs buffered input in this iteration. */
        case ZBUFFds_load: {
            size_t const result = ZBUFFv07_loadInput(zbd, &ip, iend, &notDone);
            if (ZSTDv07_isError(result))
                return result;
            if (zbd->stage != ZBUFFds_flush)
                break;
        }
            /* A decoded buffered block is flushed in the same iteration. */
        case ZBUFFds_flush:
            ZBUFFv07_flushOutput(zbd, &op, oend, &notDone);
            break;
        default:
            return ERROR(GENERIC);
        }
    }
    /* result */
    *srcSizePtr = ZBUFFv07_cursorDistance(ip, istart);
    *dstCapacityPtr = ZBUFFv07_cursorDistance(op, ostart);
    {
        size_t nextSrcSizeHint = ZSTDv07_nextSrcSizeToDecompress(zbd->zd);
        nextSrcSizeHint -= zbd->inPos; /* already loaded*/
        return nextSrcSizeHint;
    }
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Inspect production buffer ownership without dereferencing absent storage.
 * Inputs: context is a live v0.7 buffered decoder. Outputs: Returns one only when every capacity has storage. */
int sz_legacy_v07_buffers_consistent(const void* context) {
    const ZBUFFv07_DCtx* decoder = (const ZBUFFv07_DCtx*)context;
    return (decoder->inBuffSize == 0 || decoder->inBuff != NULL) &&
           (decoder->outBuffSize == 0 || decoder->outBuff != NULL);
}

/* Purpose: Observe buffer identities without reading storage contents.
 * Inputs: context is a live v0.7 decoder. Outputs: Returns borrowed buffer identities and published capacities. */
sz_legacy_buffer_state sz_legacy_v07_get_buffer_state(const void* context) {
    const ZBUFFv07_DCtx* decoder = (const ZBUFFv07_DCtx*)context;
    sz_legacy_buffer_state result = {decoder->inBuff, decoder->inBuffSize, decoder->outBuff, decoder->outBuffSize};
    return result;
}
#endif
