/* Purpose: Capture independent initialized dictionary content.
 * Inputs: Fresh decoder and complete dictionary content extent.
 * Outputs: Native status with no retained caller address. */
static size_t ZSTDvXX_refDictContent(ZSTDvXX_DCtx* dctx, const void* dict, size_t dictSize) {
    return ZSTDvXX_captureHistory(dctx, dict, dictSize, dictSize);
}
