/* Purpose: Advance the legacy bufferless decoder without retaining caller storage.
 * Inputs: Exact requested input bytes, complete output extent and an exclusive initialized context.
 * Outputs: Native byte count or error, preserving frame, checksum and skippable-state transitions. */
size_t ZSTDv06_decompressContinue(ZSTDv06_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    if (dctx->historyError)
        return dctx->historyError;

    if (srcSize > PTRDIFF_MAX || (src == NULL && srcSize != 0))
        return ERROR(srcSize_wrong);
    if (dstCapacity > PTRDIFF_MAX || (dst == NULL && dstCapacity != 0))
        return ERROR(dstSize_tooSmall);

    /* Sanity check */
    if (srcSize != dctx->expected)
        return ERROR(srcSize_wrong);

    /* Decompress : frame header; part 1 */
    switch (dctx->stage) {
    case ZSTDds_getFrameHeaderSize:
        if (srcSize != ZSTDv06_frameHeaderSize_min)
            return ERROR(srcSize_wrong); /* impossible */
        dctx->headerSize = ZSTDv06_frameHeaderSize(src, ZSTDv06_frameHeaderSize_min);
        if (ZSTDv06_isError(dctx->headerSize))
            return dctx->headerSize;
        if (ZSTDv06_copyHeader(dctx, 0, src, srcSize, ZSTDv06_frameHeaderSize_min))
            return ERROR(srcSize_wrong);
        if (dctx->headerSize > ZSTDv06_frameHeaderSize_min) {
            dctx->expected = dctx->headerSize - ZSTDv06_frameHeaderSize_min;
            dctx->stage = ZSTDds_decodeFrameHeader;
            return 0;
        }
        dctx->expected = 0; /* not necessary to copy more */
                            /* fall-through */
    case ZSTDds_decodeFrameHeader: {
        size_t result;
        if (ZSTDv06_copyHeader(dctx, ZSTDv06_frameHeaderSize_min, src, srcSize, dctx->expected))
            return ERROR(srcSize_wrong);
        result = ZSTDv06_decodeFrameHeader(dctx, dctx->headerBuffer, dctx->headerSize);
        if (ZSTDv06_isError(result))
            return result;
        dctx->expected = ZSTDv06_blockHeaderSize;
        dctx->stage = ZSTDds_decodeBlockHeader;
        return 0;
    }
    case ZSTDds_decodeBlockHeader: {
        blockProperties_t bp;
        size_t const cBlockSize = ZSTDv06_getcBlockSize(src, ZSTDv06_blockHeaderSize, &bp);
        if (ZSTDv06_isError(cBlockSize))
            return cBlockSize;
        if (bp.blockType == bt_end) {
            dctx->expected = 0;
            dctx->stage = ZSTDds_getFrameHeaderSize;
        } else {
            dctx->expected = cBlockSize;
            dctx->bType = bp.blockType;
            dctx->stage = ZSTDds_decompressBlock;
        }
        return 0;
    }
    case ZSTDds_decompressBlock:
        return ZSTDv06_decompressDataBlock(dctx, dst, dstCapacity, src, srcSize);
    default:
        return ERROR(GENERIC); /* impossible */
    }
}
