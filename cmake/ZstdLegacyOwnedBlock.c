/* Purpose: Decode a compressed raw block and capture only successful initialized output.
 * Inputs: Complete input/output extents and an initialized exclusive decoder.
 * Outputs: Native result; no caller output address is retained. */
size_t ZSTDvXX_decompressBlock(ZSTDvXX_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    size_t result;
    if (dctx->historyError)
        return dctx->historyError;
    result = ZSTDvXX_decompressBlock_internal(dctx, dst, dstCapacity, src, srcSize, (const BYTE*)dst);
    if (ZSTDvXX_isError(result))
        return result;
    {
        size_t const captured = ZSTDvXX_captureHistory(dctx, dst, dstCapacity, result);
        if (ZSTDvXX_isError(captured))
            return captured;
    }
    return result;
}
