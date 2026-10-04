/* Transactional buffers and explicit stream stages for pinned Zstandard 1.5.7.
 * The enclosing source retains the upstream dual-license notice. */

/* Purpose: Measure a public legacy byte cursor without null-pointer subtraction.
 * Inputs: begin/end delimit one borrowed live extent, or are both null when empty.
 * Outputs: Returns remaining or consumed bytes; empty null extents have distance zero. */
static size_t ZBUFFv05_cursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Acquire stream buffers with a strong ownership guarantee.
 * Inputs: The live decoder owns current buffers; requested capacities come from validated frame parameters.
 * Outputs: On error retains both original owners/capacities; on success publishes both complete replacements. */
static size_t ZBUFFv05_reserveBuffers(ZBUFFv05_DCtx* zbc, size_t inputSize, size_t outputSize) {
    char* replacementIn = NULL;
    char* replacementOut = NULL;
    /* Acquire every required replacement before publishing or releasing owners. */
    if (zbc->inBuffSize < inputSize) {
        replacementIn = (char*)malloc(inputSize);
        if (replacementIn == NULL)
            return ERROR(memory_allocation);
    }
    if (zbc->outBuffSize < outputSize) {
        replacementOut = (char*)malloc(outputSize);
        if (replacementOut == NULL) {
            free(replacementIn);
            return ERROR(memory_allocation);
        }
    }
    /* Commit complete capacity/storage pairs only after both acquisitions succeed. */
    if (replacementIn != NULL) {
        free(zbc->inBuff);
        zbc->inBuff = replacementIn;
        zbc->inBuffSize = inputSize;
    }
    if (replacementOut != NULL) {
        free(zbc->outBuff);
        zbc->outBuff = replacementOut;
        zbc->outBuffSize = outputSize;
    }
    return 0;
}

/* Purpose: Apply a complete validated frame header and prepare owned stream storage.
 * Inputs: The decoder owns its accumulated header and allocator; this call borrows no input.
 * Outputs: Returns an error or publishes the read/load stage with valid buffer ownership. */
static size_t ZBUFFv05_prepareFrame(ZBUFFv05_DCtx* zbc) {
    size_t const result = ZBUFFv05_reserveBuffers(zbc, BLOCKSIZE, (size_t)1 << zbc->params.windowLog);
    if (ZSTDv05_isError(result))
        return result;
    if (zbc->hPos) {
        /* some data already loaded into headerBuffer : transfer into inBuff */
        memcpy(zbc->inBuff, zbc->headerBuffer, zbc->hPos);
        zbc->inPos = zbc->hPos;
        zbc->hPos = 0;
        zbc->stage = ZBUFFv05ds_load;
        return 0;
    }
    zbc->stage = ZBUFFv05ds_read;
    return 0;
}

/* Purpose: Process the buffered stream read stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static size_t ZBUFFv05_readInput(ZBUFFv05_DCtx* zbc, const char** ip, const char* iend, U32* notDone) {
    size_t neededInSize = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
    if (neededInSize == 0) { /* end of frame */
        zbc->stage = ZBUFFv05ds_init;
        (*notDone) = 0;
        return 0;
    }
    if ((size_t)(ZBUFFv05_cursorDistance(iend, *ip)) >= neededInSize) {
        /* directly decode from src */
        size_t decodedSize = ZSTDv05_decompressContinue(zbc->zc, zbc->outBuff + zbc->outStart,
                                                        zbc->outBuffSize - zbc->outStart, (*ip), neededInSize);
        if (ZSTDv05_isError(decodedSize))
            return decodedSize;
        (*ip) += neededInSize;
        if (!decodedSize)
            return 0; /* this was just a header */
        zbc->outEnd = zbc->outStart + decodedSize;
        zbc->stage = ZBUFFv05ds_flush;
        return 0;
    }
    if ((*ip) == iend) {
        (*notDone) = 0;
        return 0;
    } /* no more input */
    zbc->stage = ZBUFFv05ds_load;
    return 0;
}

/* Purpose: Process the buffered stream load stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static size_t ZBUFFv05_loadInput(ZBUFFv05_DCtx* zbc, const char** ip, const char* iend, U32* notDone) {
    size_t neededInSize = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
    size_t toLoad = neededInSize - zbc->inPos; /* should always be <= remaining space within inBuff */
    size_t loadedSize;
    if (toLoad > zbc->inBuffSize - zbc->inPos)
        return ERROR(corruption_detected); /* should never happen */
    loadedSize = ZBUFFv05_limitCopy(zbc->inBuff + zbc->inPos, toLoad, (*ip), ZBUFFv05_cursorDistance(iend, *ip));
    if (loadedSize != 0)
        (*ip) += loadedSize;
    zbc->inPos += loadedSize;
    if (loadedSize < toLoad) {
        (*notDone) = 0;
        return 0;
    } /* not enough input, wait for more */
    {
        size_t decodedSize = ZSTDv05_decompressContinue(zbc->zc, zbc->outBuff + zbc->outStart,
                                                        zbc->outBuffSize - zbc->outStart, zbc->inBuff, neededInSize);
        if (ZSTDv05_isError(decodedSize))
            return decodedSize;
        zbc->inPos = 0; /* input is consumed */
        if (!decodedSize) {
            zbc->stage = ZBUFFv05ds_read;
            return 0;
        } /* this was just a header */
        zbc->outEnd = zbc->outStart + decodedSize;
        zbc->stage = ZBUFFv05ds_flush;
        /* Fall through to the flush stage. */ /* ZBUFFv05ds_flush follows */
    }
    return 0;
}

/* Purpose: Process the buffered stream flush stage.
 * Inputs: The decoder owns its buffers; cursor/end delimit borrowed live storage and notDone controls progress.
 * Outputs: Preserves native consumption, errors and stage transitions; never publishes partial buffer ownership. */
static void ZBUFFv05_flushOutput(ZBUFFv05_DCtx* zbc, char** op, const char* oend, U32* notDone) {
    size_t toFlushSize = zbc->outEnd - zbc->outStart;
    size_t flushedSize = ZBUFFv05_limitCopy((*op), ZBUFFv05_cursorDistance(oend, *op), zbc->outBuff + zbc->outStart, toFlushSize);
    if (flushedSize != 0)
        (*op) += flushedSize;
    zbc->outStart += flushedSize;
    if (flushedSize == toFlushSize) {
        zbc->stage = ZBUFFv05ds_read;
        if (zbc->outStart + BLOCKSIZE > zbc->outBuffSize)
            zbc->outStart = zbc->outEnd = 0;
        return;
    }
    /* cannot flush everything */
    (*notDone) = 0;
    return;
}

/* Purpose: Advance an initialized legacy stream through small explicit stages.
 * Inputs: The caller owns source/output and capacity counters; decoder exclusively owns its history buffers.
 * Outputs: Preserves native consumption and hints; failed acquisition retains valid owned storage. */
size_t ZBUFFv05_decompressContinue(ZBUFFv05_DCtx* zbc, void* dst, size_t* maxDstSizePtr, const void* src,
                                   size_t* srcSizePtr) {
    const char* const istart = (const char*)src;
    const char* ip = istart;
    const char* const iend = *srcSizePtr != 0 ? istart + *srcSizePtr : istart;
    char* const ostart = (char*)dst;
    char* op = ostart;
    char* const oend = *maxDstSizePtr != 0 ? ostart + *maxDstSizePtr : ostart;
    U32 notDone = 1;

    while (notDone) {
        switch (zbc->stage) {
        case ZBUFFv05ds_init:
            return ERROR(init_missing);
        case ZBUFFv05ds_readHeader:
            /* read header from src */
            {
                size_t headerSize = ZSTDv05_getFrameParams(&(zbc->params), src, *srcSizePtr);
                if (ZSTDv05_isError(headerSize))
                    return headerSize;
                if (headerSize) {
                    /* not enough input to decode header : tell how many bytes would be necessary */
                    if (*srcSizePtr != 0)
                        memcpy(zbc->headerBuffer + zbc->hPos, src, *srcSizePtr);
                    zbc->hPos += *srcSizePtr;
                    *maxDstSizePtr = 0;
                    zbc->stage = ZBUFFv05ds_loadHeader;
                    return headerSize - zbc->hPos;
                }
                zbc->stage = ZBUFFv05ds_decodeHeader;
                break;
            }
            /* fall-through */
        case ZBUFFv05ds_loadHeader:
            /* complete header from src */
            {
                size_t headerSize = ZBUFFv05_limitCopy(zbc->headerBuffer + zbc->hPos,
                                                       ZSTDv05_frameHeaderSize_max - zbc->hPos, src, *srcSizePtr);
                zbc->hPos += headerSize;
                if (headerSize != 0)
                    ip += headerSize;
                headerSize = ZSTDv05_getFrameParams(&(zbc->params), zbc->headerBuffer, zbc->hPos);
                if (ZSTDv05_isError(headerSize))
                    return headerSize;
                if (headerSize) {
                    /* not enough input to decode header : tell how many bytes would be necessary */
                    *maxDstSizePtr = 0;
                    return headerSize - zbc->hPos;
                }
                /* Fall through to header decoding without a redundant stage assignment. */ /* useless : stage follows
                                                                                             */
            }
            /* fall-through */
        case ZBUFFv05ds_decodeHeader: {
            size_t const result = ZBUFFv05_prepareFrame(zbc);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_read)
                break;
        }
            /* A complete frame preparation immediately enters the read stage. */
        case ZBUFFv05ds_read: {
            size_t const result = ZBUFFv05_readInput(zbc, &ip, iend, &notDone);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_load)
                break;
        }
            /* A partial direct read needs buffered input in this iteration. */
        case ZBUFFv05ds_load: {
            size_t const result = ZBUFFv05_loadInput(zbc, &ip, iend, &notDone);
            if (ZSTDv05_isError(result))
                return result;
            if (zbc->stage != ZBUFFv05ds_flush)
                break;
        }
            /* A decoded buffered block is flushed in the same iteration. */
        case ZBUFFv05ds_flush:
            ZBUFFv05_flushOutput(zbc, &op, oend, &notDone);
            break;
        default:
            return ERROR(GENERIC);
        }
    }
    *srcSizePtr = ZBUFFv05_cursorDistance(ip, istart);
    *maxDstSizePtr = ZBUFFv05_cursorDistance(op, ostart);

    {
        size_t nextSrcSizeHint = ZSTDv05_nextSrcSizeToDecompress(zbc->zc);
        if (nextSrcSizeHint > ZBUFFv05_blockHeaderSize)
            nextSrcSizeHint += ZBUFFv05_blockHeaderSize; /* get next block header too */
        nextSrcSizeHint -= zbc->inPos;                   /* already loaded*/
        return nextSrcSizeHint;
    }
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Inspect production buffer ownership without dereferencing absent storage.
 * Inputs: context is a live v0.5 buffered decoder. Outputs: Returns one only when every capacity has storage. */
int sz_legacy_v05_buffers_consistent(const void* context) {
    const ZBUFFv05_DCtx* decoder = (const ZBUFFv05_DCtx*)context;
    return (decoder->inBuffSize == 0 || decoder->inBuff != NULL) &&
           (decoder->outBuffSize == 0 || decoder->outBuff != NULL);
}

/* Purpose: Observe buffer identities without reading storage contents.
 * Inputs: context is a live v0.5 decoder. Outputs: Returns borrowed buffer identities and published capacities. */
sz_legacy_buffer_state sz_legacy_v05_get_buffer_state(const void* context) {
    const ZBUFFv05_DCtx* decoder = (const ZBUFFv05_DCtx*)context;
    sz_legacy_buffer_state result = {decoder->inBuff, decoder->inBuffSize, decoder->outBuff, decoder->outBuffSize};
    return result;
}
#endif
