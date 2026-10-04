/* Purpose: Decode one validated bufferless data block and capture its independent history.
 * Inputs: Exclusive context, its selected block type and complete input/output extents.
 * Outputs: Native result with the original block-stage and checksum behavior. */
static size_t ZSTDvXX_decompressDataBlock(ZSTDvXX_DCtx* dctx, void* dst, size_t dstCapacity, const void* src,
                                          size_t srcSize) {
    size_t result;
    switch (dctx->bType) {
    case bt_compressed:
        result = ZSTDvXX_decompressBlock_internal(dctx, dst, dstCapacity, src, srcSize, (const BYTE*)dst);
        break;
    case bt_raw:
        result = ZSTDvXX_copyRawBlock(dst, dstCapacity, src, srcSize);
        break;
    case bt_end:
        result = 0;
        break;
    case bt_rle:
    default:
        return ERROR(GENERIC);
    }
    dctx->stage = SUPERZIP_LEGACY_BLOCK_STAGE;
    dctx->expected = ZSTDvXX_blockHeaderSize;
    if (ZSTDvXX_isError(result))
        return result;
    {
        size_t const captured = ZSTDvXX_captureHistory(dctx, dst, dstCapacity, result);
        if (ZSTDvXX_isError(captured))
            return captured;
    }
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    if (dctx->fParams.checksumFlag)
        XXH64_update(&dctx->xxhState, dst, result);
#endif
    return result;
}
