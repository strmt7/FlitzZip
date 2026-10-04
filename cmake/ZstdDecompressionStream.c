/* Buffered stream stage refactor for pinned Zstandard 1.5.7.
 * The enclosing generated source retains its upstream license notice. */

typedef struct {
    const char* istart;
    const char* iend;
    const char* ip;
    char* ostart;
    char* oend;
    char* op;
    U32 someMoreWork;
    int returnNow;
} ZSTD_DecompressionStreamCursor;

/* Purpose: Measure a validated byte cursor without subtracting null empty-buffer pointers.
 * Inputs: begin/end borrow one live extent, or are both null for an allowed empty public buffer.
 * Outputs: Returns remaining/consumed bytes; the empty null extent has size zero. */
static size_t ZSTD_decompressionCursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Decode a stream unit into owned buffered storage or stable public output.
 * Inputs: zds owns prepared state; op/oend borrow a live output extent or both null when empty;
 * src borrows the exact requested source extent. Outputs: Returns an error or publishes bytes and next stage. */
static size_t ZSTD_decompressContinueStream(ZSTD_DStream* zds, char** op, char* oend, void const* src, size_t srcSize) {
    int const isSkipFrame = ZSTD_isSkipFrame(zds);
    if (zds->outBufferMode == ZSTD_bm_buffered) {
        size_t const dstSize = isSkipFrame ? 0 : zds->outBuffSize - zds->outStart;
        size_t const decodedSize = ZSTD_decompressContinue(zds, zds->outBuff + zds->outStart, dstSize, src, srcSize);
        FORWARD_IF_ERROR(decodedSize, "");
        if (!decodedSize && !isSkipFrame) {
            zds->streamStage = zdss_read;
        } else {
            zds->outEnd = zds->outStart + decodedSize;
            zds->streamStage = zdss_flush;
        }
    } else {
        /* Write directly into the output buffer */
        size_t const dstSize = isSkipFrame ? 0 : ZSTD_decompressionCursorDistance(oend, *op);
        size_t const decodedSize = ZSTD_decompressContinue(zds, *op, dstSize, src, srcSize);
        FORWARD_IF_ERROR(decodedSize, "");
        if (decodedSize != 0)
            *op += decodedSize;
        /* Flushing is not needed. */
        zds->streamStage = zdss_read;
        assert(*op == oend || *op <= oend);
        assert(zds->outBufferMode == ZSTD_bm_stable);
    }
    return 0;
}

/* Purpose: Accumulate a modern header or route a supported legacy frame, preserving early hints.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns a hint/error or marks a complete header; early-return and consumption behavior remain explicit. */
static size_t ZSTD_parseStreamHeader(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor, ZSTD_outBuffer* output,
                                     ZSTD_inBuffer* input, int* ready) {
    DEBUGLOG(5, "stage zdss_loadHeader (srcSize : %u)",
             (U32)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip)));
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT >= 1)
    if (zds->legacyVersion) {
        RETURN_ERROR_IF(zds->staticSize, memory_allocation, "legacy support is incompatible with static dctx");
        {
            size_t const hint = ZSTD_decompressLegacyStream(zds->legacyContext, zds->legacyVersion, output, input);
            if (hint == 0)
                zds->streamStage = zdss_init;
            {
                cursor->returnNow = 1;
                return hint;
            }
        }
    }
#endif
    {
        size_t const hSize = ZSTD_getFrameHeader_advanced(&zds->fParams, zds->headerBuffer, zds->lhSize, zds->format);
        if (zds->refMultipleDDicts && zds->ddictSet) {
            ZSTD_DCtx_selectFrameDDict(zds);
        }
        if (ZSTD_isError(hSize)) {
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT >= 1)
            U32 const legacyVersion =
                ZSTD_isLegacy(cursor->istart, ZSTD_decompressionCursorDistance(cursor->iend, cursor->istart));
            if (legacyVersion) {
                ZSTD_DDict const* const ddict = ZSTD_getDDict(zds);
                const void* const dict = ddict ? ZSTD_DDict_dictContent(ddict) : NULL;
                size_t const dictSize = ddict ? ZSTD_DDict_dictSize(ddict) : 0;
                DEBUGLOG(5, "ZSTD_decompressStream: detected legacy version v0.%u", legacyVersion);
                RETURN_ERROR_IF(zds->staticSize, memory_allocation, "legacy support is incompatible with static dctx");
                FORWARD_IF_ERROR(ZSTD_initLegacyStream(&zds->legacyContext, zds->previousLegacyVersion, legacyVersion,
                                                       dict, dictSize),
                                 "");
                zds->legacyVersion = zds->previousLegacyVersion = legacyVersion;
                {
                    size_t const hint = ZSTD_decompressLegacyStream(zds->legacyContext, legacyVersion, output, input);
                    if (hint == 0)
                        zds->streamStage = zdss_init; /* or stay in stage zdss_loadHeader */
                    {
                        cursor->returnNow = 1;
                        return hint;
                    }
                }
            }
#endif
            {
                cursor->returnNow = 1;
                return hSize;
            } /* error */
        }
        if (hSize != 0) {                              /* need more input */
            size_t const toLoad = hSize - zds->lhSize; /* if hSize!=0, hSize > zds->lhSize */
            size_t const remainingInput = (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip));
            assert(cursor->iend >= cursor->ip);
            if (toLoad > remainingInput) { /* not enough input to load full header */
                if (remainingInput > 0) {
                    ZSTD_memcpy(zds->headerBuffer + zds->lhSize, cursor->ip, remainingInput);
                    zds->lhSize += remainingInput;
                }
                input->pos = input->size;
                /* check first few bytes */
                FORWARD_IF_ERROR(
                    ZSTD_getFrameHeader_advanced(&zds->fParams, zds->headerBuffer, zds->lhSize, zds->format),
                    "First few bytes detected incorrect");
                /* return hint input size */
                {
                    cursor->returnNow = 1;
                    return (MAX((size_t)ZSTD_FRAMEHEADERSIZE_MIN(zds->format), hSize) - zds->lhSize) +
                           ZSTD_blockHeaderSize;
                } /* remaining header bytes + next block header */
            }
            assert(cursor->ip != NULL);
            ZSTD_memcpy(zds->headerBuffer + zds->lhSize, cursor->ip, toLoad);
            zds->lhSize = hSize;
            cursor->ip += toLoad;
            return 0;
        }
    }
    *ready = 1;
    return 0;
}

/* Purpose: Use the existing complete-frame shortcut only when current input and output permit it.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns an error or publishes completed-frame cursors; otherwise leaves the streaming path selected. */
static size_t ZSTD_tryStreamOneShot(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor) {
    /* check for single-pass mode opportunity */
    if (zds->fParams.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN && zds->fParams.frameType != ZSTD_skippableFrame &&
        (U64)(size_t)(ZSTD_decompressionCursorDistance(cursor->oend, cursor->op)) >= zds->fParams.frameContentSize) {
        size_t const cSize = ZSTD_findFrameCompressedSize_advanced(
            cursor->istart, (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->istart)), zds->format);
        if (cSize <= (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->istart))) {
            /* shortcut : using single-pass mode */
            size_t const decompressedSize = ZSTD_decompress_usingDDict(
                zds, cursor->op, (size_t)(ZSTD_decompressionCursorDistance(cursor->oend, cursor->op)), cursor->istart,
                cSize, ZSTD_getDDict(zds));
            if (ZSTD_isError(decompressedSize))
                return decompressedSize;
            DEBUGLOG(4, "shortcut to single-pass ZSTD_decompress_usingDDict()");
            assert(cursor->istart != NULL);
            cursor->ip = cursor->istart + cSize;
            cursor->op = cursor->op ? cursor->op + decompressedSize
                                    : cursor->op; /* can occur if frameContentSize = 0 (empty frame) */
            zds->expected = 0;
            zds->streamStage = zdss_init;
            cursor->someMoreWork = 0;
            return 0;
        }
    }
    return 0;
}

/* Purpose: Consume the validated header, enforce window limits and prepare decoder buffer capacities.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns an error or enters the read stage, preserving allocator, static-storage and stable-output contracts.
 */
static size_t ZSTD_prepareStreamFrame(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor) {
    /* Check output buffer is large enough for ZSTD_odm_stable. */
    if (zds->outBufferMode == ZSTD_bm_stable && zds->fParams.frameType != ZSTD_skippableFrame &&
        zds->fParams.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
        (U64)(size_t)(ZSTD_decompressionCursorDistance(cursor->oend, cursor->op)) < zds->fParams.frameContentSize) {
        RETURN_ERROR(dstSize_tooSmall, "ZSTD_obm_stable passed but ZSTD_outBuffer is too small");
    }

    /* Consume header (see ZSTDds_decodeFrameHeader) */
    DEBUGLOG(4, "Consume header");
    FORWARD_IF_ERROR(ZSTD_decompressBegin_usingDDict(zds, ZSTD_getDDict(zds)), "");

    if (zds->format == ZSTD_f_zstd1 && (MEM_readLE32(zds->headerBuffer) & ZSTD_MAGIC_SKIPPABLE_MASK) ==
                                           ZSTD_MAGIC_SKIPPABLE_START) { /* skippable frame */
        zds->expected = MEM_readLE32(zds->headerBuffer + ZSTD_FRAMEIDSIZE);
        zds->stage = ZSTDds_skipFrame;
    } else {
        FORWARD_IF_ERROR(ZSTD_decodeFrameHeader(zds, zds->headerBuffer, zds->lhSize), "");
        zds->expected = ZSTD_blockHeaderSize;
        zds->stage = ZSTDds_decodeBlockHeader;
    }

    /* control buffer memory usage */
    DEBUGLOG(4, "Control max memory usage (%u KB <= max %u KB)", (U32)(zds->fParams.windowSize >> 10),
             (U32)(zds->maxWindowSize >> 10));
    zds->fParams.windowSize = MAX(zds->fParams.windowSize, 1U << ZSTD_WINDOWLOG_ABSOLUTEMIN);
    RETURN_ERROR_IF(zds->fParams.windowSize > zds->maxWindowSize, frameParameter_windowTooLarge, "");
    if (zds->maxBlockSizeParam != 0)
        zds->fParams.blockSizeMax = MIN(zds->fParams.blockSizeMax, (unsigned)zds->maxBlockSizeParam);

    /* Adapt buffer sizes to frame header instructions */
    {
        size_t const neededInBuffSize = MAX(zds->fParams.blockSizeMax, 4 /* frame checksum */);
        size_t const neededOutBuffSize =
            zds->outBufferMode == ZSTD_bm_buffered
                ? ZSTD_decodingBufferSize_internal(zds->fParams.windowSize, zds->fParams.frameContentSize,
                                                   zds->fParams.blockSizeMax)
                : 0;

        ZSTD_DCtx_updateOversizedDuration(zds, neededInBuffSize, neededOutBuffSize);

        {
            int const tooSmall = (zds->inBuffSize < neededInBuffSize) || (zds->outBuffSize < neededOutBuffSize);
            int const tooLarge = ZSTD_DCtx_isOversizedTooLong(zds);

            if (tooSmall || tooLarge) {
                size_t const bufferSize = neededInBuffSize + neededOutBuffSize;
                DEBUGLOG(4, "inBuff  : from %u to %u", (U32)zds->inBuffSize, (U32)neededInBuffSize);
                DEBUGLOG(4, "outBuff : from %u to %u", (U32)zds->outBuffSize, (U32)neededOutBuffSize);
                if (zds->staticSize) { /* static DCtx */
                    DEBUGLOG(4, "staticSize : %u", (U32)zds->staticSize);
                    assert(zds->staticSize >= sizeof(ZSTD_DCtx)); /* controlled at init */
                    RETURN_ERROR_IF(bufferSize > zds->staticSize - sizeof(ZSTD_DCtx), memory_allocation, "");
                } else {
                    ZSTD_customFree(zds->inBuff, zds->customMem);
                    zds->inBuffSize = 0;
                    zds->outBuffSize = 0;
                    zds->inBuff = (char*)ZSTD_customMalloc(bufferSize, zds->customMem);
                    RETURN_ERROR_IF(zds->inBuff == NULL, memory_allocation, "");
                }
                zds->inBuffSize = neededInBuffSize;
                zds->outBuff = zds->inBuff + zds->inBuffSize;
                zds->outBuffSize = neededOutBuffSize;
            }
        }
    }
    zds->streamStage = zdss_read;
    return 0;
}

/* Purpose: Decode directly available input or select accumulation when a complete unit is unavailable.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns an error or advances cursors/stage; stops work on exhausted input or a completed frame. */
static size_t ZSTD_readStreamInput(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor) {
    DEBUGLOG(5, "stage zdss_read");
    {
        size_t const neededInSize = ZSTD_nextSrcSizeToDecompressWithInputSize(
            zds, (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip)));
        DEBUGLOG(5, "neededInSize = %u", (U32)neededInSize);
        if (neededInSize == 0) { /* end of frame */
            zds->streamStage = zdss_init;
            cursor->someMoreWork = 0;
            return 0;
        }
        if ((size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip)) >=
            neededInSize) { /* decode directly from src */
            FORWARD_IF_ERROR(ZSTD_decompressContinueStream(zds, &cursor->op, cursor->oend, cursor->ip, neededInSize),
                             "");
            assert(cursor->ip != NULL);
            cursor->ip += neededInSize;
            /* Function modifies the stage so we must break */
            return 0;
        }
    }
    if (cursor->ip == cursor->iend) {
        cursor->someMoreWork = 0;
        return 0;
    } /* no more input */
    zds->streamStage = zdss_load;
    return 0;
}

/* Purpose: Accumulate a complete compressed unit or skip payload before advancing decoder state.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns an error or publishes bounded consumption and the next read/flush stage. */
static size_t ZSTD_loadStreamInput(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor) {
    {
        size_t const neededInSize = ZSTD_nextSrcSizeToDecompress(zds);
        size_t const toLoad = neededInSize - zds->inPos;
        int const isSkipFrame = ZSTD_isSkipFrame(zds);
        size_t loadedSize;
        /* At this point we shouldn't be decompressing a block that we can stream. */
        assert(neededInSize == ZSTD_nextSrcSizeToDecompressWithInputSize(
                                   zds, (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip))));
        if (isSkipFrame) {
            loadedSize = MIN(toLoad, (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip)));
        } else {
            RETURN_ERROR_IF(toLoad > zds->inBuffSize - zds->inPos, corruption_detected, "should never happen");
            loadedSize = ZSTD_limitCopy(zds->inBuff + zds->inPos, toLoad, cursor->ip,
                                        (size_t)(ZSTD_decompressionCursorDistance(cursor->iend, cursor->ip)));
        }
        if (loadedSize != 0) {
            /* ip may be NULL */
            cursor->ip += loadedSize;
            zds->inPos += loadedSize;
        }
        if (loadedSize < toLoad) {
            cursor->someMoreWork = 0;
            return 0;
        } /* not enough input, wait for more */

        /* decode loaded input */
        zds->inPos = 0; /* input is consumed */
        FORWARD_IF_ERROR(ZSTD_decompressContinueStream(zds, &cursor->op, cursor->oend, zds->inBuff, neededInSize), "");
        /* Function modifies the stage so we must break */
        return 0;
    }
}

/* Purpose: Flush decoded bytes while preserving the rolling output window and backpressure.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Publishes produced bytes and read/flush progress; stops when the public output cannot accept more. */
static void ZSTD_flushStreamOutput(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor) {
    {
        size_t const toFlushSize = zds->outEnd - zds->outStart;
        size_t const flushedSize =
            ZSTD_limitCopy(cursor->op, (size_t)(ZSTD_decompressionCursorDistance(cursor->oend, cursor->op)),
                           zds->outBuff + zds->outStart, toFlushSize);

        cursor->op = cursor->op ? cursor->op + flushedSize : cursor->op;

        zds->outStart += flushedSize;
        if (flushedSize == toFlushSize) { /* flush completed */
            zds->streamStage = zdss_read;
            if ((zds->outBuffSize < zds->fParams.frameContentSize) &&
                (zds->outStart + zds->fParams.blockSizeMax > zds->outBuffSize)) {
                DEBUGLOG(5, "restart filling outBuff from beginning (left:%i, needed:%u)",
                         (int)(zds->outBuffSize - zds->outStart), (U32)zds->fParams.blockSizeMax);
                zds->outStart = zds->outEnd = 0;
            }
            return;
        }
    }
    /* cannot complete flush */
    cursor->someMoreWork = 0;
    return;
}

/* Purpose: Publish public positions and preserve no-progress, input-hint and hostage-byte accounting.
 * Inputs: zds owns validated decoder state; cursor borrows live input/output extents;
 * optional public buffers and flags remain live through this operation.
 * Outputs: Returns completion, remaining-input hint or a native no-progress error with the original public positions.
 */
static size_t ZSTD_finishStreamCall(ZSTD_DStream* zds, ZSTD_DecompressionStreamCursor* cursor, ZSTD_outBuffer* output,
                                    ZSTD_inBuffer* input) {
    /* result */
    input->pos = (size_t)(ZSTD_decompressionCursorDistance(cursor->ip, (const char*)(input->src)));
    output->pos = (size_t)(ZSTD_decompressionCursorDistance(cursor->op, (char*)(output->dst)));

    /* Update the expected output buffer for ZSTD_obm_stable. */
    zds->expectedOutBuffer = *output;

    if ((cursor->ip == cursor->istart) && (cursor->op == cursor->ostart)) { /* no forward progress */
        zds->noForwardProgress++;
        if (zds->noForwardProgress >= ZSTD_NO_FORWARD_PROGRESS_MAX) {
            RETURN_ERROR_IF(cursor->op == cursor->oend, noForwardProgress_destFull, "");
            RETURN_ERROR_IF(cursor->ip == cursor->iend, noForwardProgress_inputEmpty, "");
            assert(0);
        }
    } else {
        zds->noForwardProgress = 0;
    }
    {
        size_t nextSrcSizeHint = ZSTD_nextSrcSizeToDecompress(zds);
        if (!nextSrcSizeHint) {                 /* frame fully decoded */
            if (zds->outEnd == zds->outStart) { /* output fully flushed */
                if (zds->hostageByte) {
                    if (input->pos >= input->size) {
                        /* can't release hostage (not present) */
                        zds->streamStage = zdss_read;
                        return 1;
                    }
                    input->pos++; /* release hostage */
                } /* zds->hostageByte */
                return 0;
            } /* zds->outEnd == zds->outStart */
            if (!zds->hostageByte) { /* output not fully flushed; keep last byte as hostage; will be released when all
                                        output is flushed */
                input->pos--;        /* note : pos > 0, otherwise, impossible to finish reading last block */
                zds->hostageByte = 1;
            }
            return 1;
        } /* nextSrcSizeHint==0 */
        nextSrcSizeHint +=
            ZSTD_blockHeaderSize * (ZSTD_nextInputType(zds) == ZSTDnit_block); /* preload header of next block */
        assert(zds->inPos <= nextSrcSizeHint);
        nextSrcSizeHint -= zds->inPos; /* part already loaded*/
        return nextSrcSizeHint;
    }
}

/* Purpose: Route validated public streaming buffers through explicit header, read, load and flush stages.
 * Inputs: zds owns decoder state; input/output borrow their declared storage and bounded positions.
 * Outputs: Returns a native hint/error and preserves streaming consumption, stable buffers and frame support. */
size_t ZSTD_decompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output, ZSTD_inBuffer* input) {
    ZSTD_DecompressionStreamCursor cursor;
    const char* const src = (const char*)input->src;
    char* const dst = (char*)output->dst;
    DEBUGLOG(5, "ZSTD_decompressStream");
    assert(zds != NULL);
    RETURN_ERROR_IF(input->pos > input->size, srcSize_wrong, "forbidden. in: pos: %u   vs size: %u", (U32)input->pos,
                    (U32)input->size);
    RETURN_ERROR_IF(output->pos > output->size, dstSize_tooSmall, "forbidden. out: pos: %u   vs size: %u",
                    (U32)output->pos, (U32)output->size);
    /* Validate positions before forming any buffer-offset pointers. */
    cursor.istart = input->pos != 0 ? src + input->pos : src;
    cursor.iend = input->size != 0 ? src + input->size : src;
    cursor.ip = cursor.istart;
    cursor.ostart = output->pos != 0 ? dst + output->pos : dst;
    cursor.oend = output->size != 0 ? dst + output->size : dst;
    cursor.op = cursor.ostart;
    cursor.someMoreWork = 1;
    cursor.returnNow = 0;
    DEBUGLOG(5, "input size : %u", (U32)(input->size - input->pos));
    FORWARD_IF_ERROR(ZSTD_checkOutBuffer(zds, output), "");
    while (cursor.someMoreWork) {
        switch (zds->streamStage) {
        case zdss_init:
            DEBUGLOG(5, "stage zdss_init => transparent reset ");
            zds->streamStage = zdss_loadHeader;
            zds->lhSize = zds->inPos = zds->outStart = zds->outEnd = 0;
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT >= 1)
            zds->legacyVersion = 0;
#endif
            zds->hostageByte = 0;
            zds->expectedOutBuffer = *output;
            ZSTD_FALLTHROUGH;
        case zdss_loadHeader: {
            int ready = 0;
            size_t const result = ZSTD_parseStreamHeader(zds, &cursor, output, input, &ready);
            if (ZSTD_isError(result) || cursor.returnNow)
                return result;
            if (!ready)
                break;
            FORWARD_IF_ERROR(ZSTD_tryStreamOneShot(zds, &cursor), "");
            if (!cursor.someMoreWork)
                break;
            FORWARD_IF_ERROR(ZSTD_prepareStreamFrame(zds, &cursor), "");
            ZSTD_FALLTHROUGH;
        }
        case zdss_read:
            FORWARD_IF_ERROR(ZSTD_readStreamInput(zds, &cursor), "");
            if (zds->streamStage != zdss_load)
                break;
            ZSTD_FALLTHROUGH;
        case zdss_load:
            FORWARD_IF_ERROR(ZSTD_loadStreamInput(zds, &cursor), "");
            break;
        case zdss_flush:
            ZSTD_flushStreamOutput(zds, &cursor);
            break;
        default:
            assert(0);
            RETURN_ERROR(GENERIC, "impossible to reach");
        }
    }
    return ZSTD_finishStreamCall(zds, &cursor, output, input);
}
