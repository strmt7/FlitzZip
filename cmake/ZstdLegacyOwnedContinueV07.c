/* Purpose: Advance the legacy bufferless decoder without retaining caller storage.
 * Inputs: Exact requested input bytes, complete output extent and an exclusive initialized context.
 * Outputs: Native byte count or error, preserving frame, checksum and skippable-state transitions. */
size_t ZSTDv07_decompressContinue(ZSTDv07_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    if (dctx->historyError)
        return dctx->historyError;

    if (srcSize > PTRDIFF_MAX || (src == NULL && srcSize != 0))
        return ERROR(srcSize_wrong);
    if (dstCapacity > PTRDIFF_MAX || (dst == NULL && dstCapacity != 0))
        return ERROR(dstSize_tooSmall);

    /* Sanity check */
    if (srcSize != dctx->expected)
        return ERROR(srcSize_wrong);

    switch (dctx->stage) {
    case ZSTDds_getFrameHeaderSize:
        if (srcSize != ZSTDv07_frameHeaderSize_min)
            return ERROR(srcSize_wrong); /* impossible */
        if ((MEM_readLE32(src) & 0xFFFFFFF0U) == ZSTDv07_MAGIC_SKIPPABLE_START) {
            if (ZSTDv07_copyHeader(dctx, 0, src, srcSize, ZSTDv07_frameHeaderSize_min))
                return ERROR(srcSize_wrong);
            dctx->expected =
                ZSTDv07_skippableHeaderSize - ZSTDv07_frameHeaderSize_min; /* magic number + skippable frame length */
            dctx->stage = ZSTDds_decodeSkippableHeader;
            return 0;
        }
        dctx->headerSize = ZSTDv07_frameHeaderSize(src, ZSTDv07_frameHeaderSize_min);
        if (ZSTDv07_isError(dctx->headerSize))
            return dctx->headerSize;
        if (ZSTDv07_copyHeader(dctx, 0, src, srcSize, ZSTDv07_frameHeaderSize_min))
            return ERROR(srcSize_wrong);
        if (dctx->headerSize > ZSTDv07_frameHeaderSize_min) {
            dctx->expected = dctx->headerSize - ZSTDv07_frameHeaderSize_min;
            dctx->stage = ZSTDds_decodeFrameHeader;
            return 0;
        }
        dctx->expected = 0; /* not necessary to copy more */
                            /* fall-through */
    case ZSTDds_decodeFrameHeader: {
        size_t result;
        if (ZSTDv07_copyHeader(dctx, ZSTDv07_frameHeaderSize_min, src, srcSize, dctx->expected))
            return ERROR(srcSize_wrong);
        result = ZSTDv07_decodeFrameHeader(dctx, dctx->headerBuffer, dctx->headerSize);
        if (ZSTDv07_isError(result))
            return result;
        dctx->expected = ZSTDv07_blockHeaderSize;
        dctx->stage = ZSTDds_decodeBlockHeader;
        return 0;
    }
    case ZSTDds_decodeBlockHeader: {
        blockProperties_t bp;
        size_t const cBlockSize = ZSTDv07_getcBlockSize(src, ZSTDv07_blockHeaderSize, &bp);
        if (ZSTDv07_isError(cBlockSize))
            return cBlockSize;
        if (bp.blockType == bt_end) {
            if (dctx->fParams.checksumFlag) {
                U64 const h64 = XXH64_digest(&dctx->xxhState);
                U32 const h32 = (U32)(h64 >> 11) & ((1 << 22) - 1);
                const BYTE* const ip = (const BYTE*)src;
                U32 const check32 = ip[2] + (ip[1] << 8) + ((ip[0] & 0x3F) << 16);
                if (check32 != h32)
                    return ERROR(checksum_wrong);
            }
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
        return ZSTDv07_decompressDataBlock(dctx, dst, dstCapacity, src, srcSize);
    case ZSTDds_decodeSkippableHeader: {
        if (ZSTDv07_copyHeader(dctx, ZSTDv07_frameHeaderSize_min, src, srcSize, dctx->expected))
            return ERROR(srcSize_wrong);
        dctx->expected = MEM_readLE32(dctx->headerBuffer + 4);
        dctx->stage = ZSTDds_skipFrame;
        return 0;
    }
    case ZSTDds_skipFrame: {
        dctx->expected = 0;
        dctx->stage = ZSTDds_getFrameHeaderSize;
        return 0;
    }
    default:
        return ERROR(GENERIC); /* impossible */
    }
}
