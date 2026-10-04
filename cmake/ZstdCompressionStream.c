/* Streaming compressor stage refactor for pinned Zstandard 1.5.7.
 * The enclosing generated source retains its upstream license notice. */

typedef struct {
    const char* ip;
    const char* iend;
    char* op;
    char* oend;
    U32 someMoreWork;
} ZSTD_CompressionStreamCursor;

/* Purpose: Measure a validated byte cursor without subtracting null empty-buffer pointers.
 * Inputs: begin/end borrow one live extent, or are both null for an allowed empty public buffer.
 * Outputs: Returns remaining/consumed bytes; the empty null extent has size zero. */
static size_t ZSTD_compressionCursorDistance(const char* end, const char* begin) {
    if (begin == NULL) {
        assert(end == NULL);
        return 0;
    }
    return (size_t)(end - begin);
}

/* Purpose: Load a buffered block or retain a stable-input remainder, honoring flush/end shortcuts.
 * Inputs: zcs owns stream buffers; cursor borrows live extents; flushMode is the caller directive.
 * Outputs: Returns a native error or stage control; publishes the same cursor, frame and buffer transitions. */
static size_t ZSTD_prepareStreamInput(ZSTD_CStream* zcs, ZSTD_CompressionStreamCursor* cursor,
                                      ZSTD_EndDirective flushMode) {
    if ((flushMode == ZSTD_e_end) &&
        ((size_t)(ZSTD_compressionCursorDistance(cursor->oend, cursor->op)) >=
             ZSTD_compressBound(
                 (size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip))) /* Enough output space */
         || zcs->appliedParams.outBufferMode == ZSTD_bm_stable) /* OR we are allowed to return dstSizeTooSmall */
        && (zcs->inBuffPos == 0)) {
        /* shortcut to compression pass directly into output buffer */
        size_t const cSize =
            ZSTD_compressEnd_public(zcs, cursor->op, (size_t)(ZSTD_compressionCursorDistance(cursor->oend, cursor->op)),
                                    cursor->ip, (size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip)));
        DEBUGLOG(4, "ZSTD_compressEnd : cSize=%u", (unsigned)cSize);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressEnd failed");
        cursor->ip = cursor->iend;
        if (cSize != 0)
            cursor->op += cSize;
        zcs->frameEnded = 1;
        ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
        cursor->someMoreWork = 0;
        return 0;
    }
    /* complete loading into inBuffer in buffered mode */
    if (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered) {
        size_t const toLoad = zcs->inBuffTarget - zcs->inBuffPos;
        size_t const loaded = ZSTD_limitCopy(zcs->inBuff + zcs->inBuffPos, toLoad, cursor->ip,
                                             (size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip)));
        zcs->inBuffPos += loaded;
        if (cursor->ip)
            cursor->ip += loaded;
        if ((flushMode == ZSTD_e_continue) && (zcs->inBuffPos < zcs->inBuffTarget)) {
            /* not enough input to fill full block : stop here */
            cursor->someMoreWork = 0;
            return 0;
        }
        if ((flushMode == ZSTD_e_flush) && (zcs->inBuffPos == zcs->inToCompress)) {
            /* empty */
            cursor->someMoreWork = 0;
            return 0;
        }
    } else {
        assert(zcs->appliedParams.inBufferMode == ZSTD_bm_stable);
        if ((flushMode == ZSTD_e_continue) &&
            ((size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip)) < zcs->blockSizeMax)) {
            /* can't compress a full block : stop here */
            zcs->stableIn_notConsumed = (size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip));
            cursor->ip = cursor->iend; /* pretend to have consumed input */
            cursor->someMoreWork = 0;
            return 0;
        }
        if ((flushMode == ZSTD_e_flush) && (cursor->ip == cursor->iend)) {
            /* empty */
            cursor->someMoreWork = 0;
            return 0;
        }
    }
    return 1;
}

/* Purpose: Compress a complete block and choose direct output or the owned flush buffer.
 * Inputs: zcs owns stream buffers; cursor borrows live extents; flushMode is the caller directive.
 * Outputs: Returns a native error or stage control; publishes the same cursor, frame and buffer transitions. */
static size_t ZSTD_compressStreamBlock(ZSTD_CStream* zcs, ZSTD_CompressionStreamCursor* cursor,
                                       ZSTD_EndDirective flushMode) {
    /* compress current block (note : this stage cannot be stopped in the middle) */
    DEBUGLOG(5, "stream compression stage (flushMode==%u)", flushMode);
    {
        int const inputBuffered = (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered);
        void* cDst;
        size_t cSize;
        size_t oSize = (size_t)(ZSTD_compressionCursorDistance(cursor->oend, cursor->op));
        size_t const iSize =
            inputBuffered ? zcs->inBuffPos - zcs->inToCompress
                          : MIN((size_t)(ZSTD_compressionCursorDistance(cursor->iend, cursor->ip)), zcs->blockSizeMax);
        if (oSize >= ZSTD_compressBound(iSize) || zcs->appliedParams.outBufferMode == ZSTD_bm_stable)
            cDst = cursor->op; /* compress into output buffer, to skip flush stage */
        else
            cDst = zcs->outBuff, oSize = zcs->outBuffSize;
        if (inputBuffered) {
            unsigned const lastBlock = (flushMode == ZSTD_e_end) && (cursor->ip == cursor->iend);
            cSize = lastBlock ? ZSTD_compressEnd_public(zcs, cDst, oSize, zcs->inBuff + zcs->inToCompress, iSize)
                              : ZSTD_compressContinue_public(zcs, cDst, oSize, zcs->inBuff + zcs->inToCompress, iSize);
            FORWARD_IF_ERROR(cSize, "%s", lastBlock ? "ZSTD_compressEnd failed" : "ZSTD_compressContinue failed");
            zcs->frameEnded = lastBlock;
            /* prepare next block */
            zcs->inBuffTarget = zcs->inBuffPos + zcs->blockSizeMax;
            if (zcs->inBuffTarget > zcs->inBuffSize)
                zcs->inBuffPos = 0, zcs->inBuffTarget = zcs->blockSizeMax;
            DEBUGLOG(5, "inBuffTarget:%u / inBuffSize:%u", (unsigned)zcs->inBuffTarget, (unsigned)zcs->inBuffSize);
            if (!lastBlock)
                assert(zcs->inBuffTarget <= zcs->inBuffSize);
            zcs->inToCompress = zcs->inBuffPos;
        } else { /* !inputBuffered, hence ZSTD_bm_stable */
            unsigned const lastBlock =
                (flushMode == ZSTD_e_end) && (ZSTD_compressionCursorDistance(cursor->iend, cursor->ip) == iSize);
            cSize = lastBlock ? ZSTD_compressEnd_public(zcs, cDst, oSize, cursor->ip, iSize)
                              : ZSTD_compressContinue_public(zcs, cDst, oSize, cursor->ip, iSize);
            /* Consume the input prior to error checking to mirror buffered mode. */
            if (cursor->ip)
                cursor->ip += iSize;
            FORWARD_IF_ERROR(cSize, "%s", lastBlock ? "ZSTD_compressEnd failed" : "ZSTD_compressContinue failed");
            zcs->frameEnded = lastBlock;
            if (lastBlock)
                assert(cursor->ip == cursor->iend);
        }
        if (cDst == cursor->op) { /* no need to flush */
            if (cSize != 0)
                cursor->op += cSize;
            if (zcs->frameEnded) {
                DEBUGLOG(5, "Frame completed directly in outBuffer");
                cursor->someMoreWork = 0;
                ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
            }
            return 0;
        }
        zcs->outBuffContentSize = cSize;
        zcs->outBuffFlushedSize = 0;
        zcs->streamStage = zcss_flush; /* pass-through to flush stage */
    }
    return 0;
}

/* Purpose: Copy pending encoded bytes and finish or resume the compression stage.
 * Inputs: zcs owns validated stream buffers; cursor borrows the live output extent.
 * Outputs: Returns a native error or stage control; publishes the same cursor, frame and buffer transitions. */
static size_t ZSTD_flushStreamOutput(ZSTD_CStream* zcs, ZSTD_CompressionStreamCursor* cursor) {
    DEBUGLOG(5, "flush stage");
    assert(zcs->appliedParams.outBufferMode == ZSTD_bm_buffered);
    {
        size_t const toFlush = zcs->outBuffContentSize - zcs->outBuffFlushedSize;
        size_t const flushed =
            ZSTD_limitCopy(cursor->op, (size_t)(ZSTD_compressionCursorDistance(cursor->oend, cursor->op)),
                           zcs->outBuff + zcs->outBuffFlushedSize, toFlush);
        DEBUGLOG(5, "toFlush: %u into %u ==> flushed: %u", (unsigned)toFlush,
                 (unsigned)(ZSTD_compressionCursorDistance(cursor->oend, cursor->op)), (unsigned)flushed);
        if (flushed)
            cursor->op += flushed;
        zcs->outBuffFlushedSize += flushed;
        if (toFlush != flushed) {
            /* flush not fully completed, presumably because dst is too small */
            assert(cursor->op == cursor->oend);
            cursor->someMoreWork = 0;
            return 0;
        }
        zcs->outBuffContentSize = zcs->outBuffFlushedSize = 0;
        if (zcs->frameEnded) {
            DEBUGLOG(5, "Frame completed on flush");
            cursor->someMoreWork = 0;
            ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
            return 0;
        }
        zcs->streamStage = zcss_load;
        return 0;
    }
}

/* Purpose: Route synchronous streaming compression through its load, block and flush operations.
 * Inputs: zcs owns initialized state; input/output borrow validated buffers; flushMode controls frame completion.
 * Outputs: Returns an input-size hint or error and preserves consumed/produced positions and stable-buffer semantics.
 */
static size_t ZSTD_compressStream_generic(ZSTD_CStream* zcs, ZSTD_outBuffer* output, ZSTD_inBuffer* input,
                                          ZSTD_EndDirective const flushMode) {
    const char* const istart = (assert(input != NULL), (const char*)input->src);
    char* const ostart = (assert(output != NULL), (char*)output->dst);
    ZSTD_CompressionStreamCursor cursor = {
        (istart != NULL) ? istart + input->pos : istart, (istart != NULL) ? istart + input->size : istart,
        (ostart != NULL) ? ostart + output->pos : ostart, (ostart != NULL) ? ostart + output->size : ostart, 1};

    /* check expectations */
    DEBUGLOG(5, "ZSTD_compressStream_generic, flush=%i, srcSize = %zu", (int)flushMode, input->size - input->pos);
    assert(zcs != NULL);
    if (zcs->appliedParams.inBufferMode == ZSTD_bm_stable) {
        assert(input->pos >= zcs->stableIn_notConsumed);
        input->pos -= zcs->stableIn_notConsumed;
        if (cursor.ip)
            cursor.ip -= zcs->stableIn_notConsumed;
        zcs->stableIn_notConsumed = 0;
    }
    if (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered) {
        assert(zcs->inBuff != NULL);
        assert(zcs->inBuffSize > 0);
    }
    if (zcs->appliedParams.outBufferMode == ZSTD_bm_buffered) {
        assert(zcs->outBuff != NULL);
        assert(zcs->outBuffSize > 0);
    }
    if (input->src == NULL)
        assert(input->size == 0);
    assert(input->pos <= input->size);
    if (output->dst == NULL)
        assert(output->size == 0);
    assert(output->pos <= output->size);
    assert((U32)flushMode <= (U32)ZSTD_e_end);
    while (cursor.someMoreWork) {
        switch (zcs->streamStage) {
        case zcss_init:
            RETURN_ERROR(init_missing, "call ZSTD_initCStream() first!");
        case zcss_load: {
            size_t const ready = ZSTD_prepareStreamInput(zcs, &cursor, flushMode);
            FORWARD_IF_ERROR(ready, "");
            if (!ready)
                break;
            FORWARD_IF_ERROR(ZSTD_compressStreamBlock(zcs, &cursor, flushMode), "");
            if (zcs->streamStage != zcss_flush)
                break;
            ZSTD_FALLTHROUGH;
        }
        case zcss_flush:
            FORWARD_IF_ERROR(ZSTD_flushStreamOutput(zcs, &cursor), "");
            break;
        default:
            assert(0);
        }
    }
    input->pos = (size_t)(ZSTD_compressionCursorDistance(cursor.ip, istart));
    output->pos = (size_t)(ZSTD_compressionCursorDistance(cursor.op, ostart));
    if (zcs->frameEnded)
        return 0;
    return ZSTD_nextInputSizeHint(zcs);
}
