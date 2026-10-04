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
