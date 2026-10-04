/* Purpose: Capture a frame-header segment in the decoder's actual bounded header array.
 * Inputs: Complete source extent, exact consumed count and destination offset under the current stage.
 * Outputs: Native status; invalid geometry makes no pointer or copy and no source address is retained. */
static size_t ZSTDvXX_copyHeader(ZSTDvXX_DCtx* dctx, size_t offset, const void* source, size_t capacity, size_t count) {
    if (ZBUFF_copyBytes(dctx->headerBuffer, sizeof(dctx->headerBuffer), offset, source, capacity, 0, count) !=
        ZBUFF_buffer_ok)
        return ERROR(srcSize_wrong);
    return 0;
}
