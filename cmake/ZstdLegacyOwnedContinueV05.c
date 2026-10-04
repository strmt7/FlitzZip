/* Purpose: Advance the legacy bufferless decoder without retaining caller storage.
 * Inputs: Exact requested input bytes, complete output extent and an exclusive initialized context.
 * Outputs: Native byte count or error, preserving frame, checksum and skippable-state transitions. */
size_t ZSTDv05_decompressContinue(ZSTDv05_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
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
    case ZSTDv05ds_getFrameHeaderSize:
        /* get frame header size */
        if (srcSize != ZSTDv05_frameHeaderSize_min)
            return ERROR(srcSize_wrong); /* impossible */
        dctx->headerSize = ZSTDv05_decodeFrameHeader_Part1(dctx, src, ZSTDv05_frameHeaderSize_min);
        if (ZSTDv05_isError(dctx->headerSize))
            return dctx->headerSize;
        if (ZSTDv05_copyHeader(dctx, 0, src, srcSize, ZSTDv05_frameHeaderSize_min))
            return ERROR(srcSize_wrong);
        if (dctx->headerSize > ZSTDv05_frameHeaderSize_min)
            return ERROR(GENERIC); /* should never happen */
        dctx->expected = 0;        /* not necessary to copy more */
        /* fallthrough */
    case ZSTDv05ds_decodeFrameHeader:
        /* get frame header */
        {
            size_t const result = ZSTDv05_decodeFrameHeader_Part2(dctx, dctx->headerBuffer, dctx->headerSize);
            if (ZSTDv05_isError(result))
                return result;
            dctx->expected = ZSTDv05_blockHeaderSize;
            dctx->stage = ZSTDv05ds_decodeBlockHeader;
            return 0;
        }
    case ZSTDv05ds_decodeBlockHeader: {
        /* Decode block header */
        blockProperties_t bp;
        size_t blockSize = ZSTDv05_getcBlockSize(src, ZSTDv05_blockHeaderSize, &bp);
        if (ZSTDv05_isError(blockSize))
            return blockSize;
        if (bp.blockType == bt_end) {
            dctx->expected = 0;
            dctx->stage = ZSTDv05ds_getFrameHeaderSize;
        } else {
            dctx->expected = blockSize;
            dctx->bType = bp.blockType;
            dctx->stage = ZSTDv05ds_decompressBlock;
        }
        return 0;
    }
    case ZSTDv05ds_decompressBlock:
        return ZSTDv05_decompressDataBlock(dctx, dst, dstCapacity, src, srcSize);
    default:
        return ERROR(GENERIC); /* impossible */
    }
}
