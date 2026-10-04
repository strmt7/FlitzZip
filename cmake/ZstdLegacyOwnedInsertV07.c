/* Purpose: Capture an initialized uncompressed block in independent history.
 * Inputs: Exclusive initialized decoder and the block's complete readable extent.
 * Outputs: Native byte count or error; no block pointer is retained. */
ZSTDLIBv07_API size_t ZSTDv07_insertBlock(ZSTDv07_DCtx* dctx, const void* blockStart, size_t blockSize) {
    if (dctx->historyError)
        return dctx->historyError;
    {
        size_t const captured = ZSTDv07_captureHistory(dctx, blockStart, blockSize, blockSize);
        if (ZSTDv07_isError(captured))
            return captured;
    }
    return blockSize;
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Check error propagation and valid history after a malformed canonical block call.
 * Inputs: None; all decoder and byte storage belongs to this bounded serial probe.
 * Outputs: Returns one only when the malformed call fails and the history end remains the destination start. */
int sz_legacy_v07_block_error_history(void) {
    ZSTDv07_DCtx* context = ZSTDv07_createDCtx();
    BYTE destination[64], malformed[1] = {0};
    size_t result;
    int preserved;
    if (context == NULL)
        return 0;
    result = ZSTDv07_decompressBlock(context, destination, sizeof(destination), malformed, sizeof(malformed));
    preserved = ZSTDv07_isError(result) && ZBUFF_ownedHistorySize(context->history) == 0;
    ZSTDv07_freeDCtx(context);
    return preserved;
}
#endif
