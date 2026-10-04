/* Bufferless state-machine refactor for pinned Zstandard 1.5.7.
 * The enclosing generated source retains its upstream license notice. */

/* Purpose: Read the frame prefix and select a normal or skippable header.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_readFrameHeaderPrefix(ZSTD_DCtx* dctx, const void* src, size_t srcSize) {
    assert(src != NULL);
    if (dctx->format == ZSTD_f_zstd1) {      /* allows header */
        assert(srcSize >= ZSTD_FRAMEIDSIZE); /* to read skippable magic number */
        if ((MEM_readLE32(src) & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START) { /* skippable frame */
            ZSTD_memcpy(dctx->headerBuffer, src, srcSize);
            dctx->expected =
                ZSTD_SKIPPABLEHEADERSIZE - srcSize; /* remaining to load to get full skippable frame header */
            dctx->stage = ZSTDds_decodeSkippableHeader;
            return 0;
        }
    }
    dctx->headerSize = ZSTD_frameHeaderSize_internal(src, srcSize, dctx->format);
    if (ZSTD_isError(dctx->headerSize))
        return dctx->headerSize;
    ZSTD_memcpy(dctx->headerBuffer, src, srcSize);
    dctx->expected = dctx->headerSize - srcSize;
    dctx->stage = ZSTDds_decodeFrameHeader;
    return 0;
}

/* Purpose: Complete the frame header and enter block-header decoding.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_readFrameHeaderSuffix(ZSTD_DCtx* dctx, const void* src, size_t srcSize) {
    assert(src != NULL);
    ZSTD_memcpy(dctx->headerBuffer + (dctx->headerSize - srcSize), src, srcSize);
    FORWARD_IF_ERROR(ZSTD_decodeFrameHeader(dctx, dctx->headerBuffer, dctx->headerSize), "");
    dctx->expected = ZSTD_blockHeaderSize;
    dctx->stage = ZSTDds_decodeBlockHeader;
    return 0;
}

/* Purpose: Validate a block header and select its payload or frame trailer.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_readBlockHeader(ZSTD_DCtx* dctx, const void* src) {
    {
        blockProperties_t bp;
        size_t const cBlockSize = ZSTD_getcBlockSize(src, ZSTD_blockHeaderSize, &bp);
        if (ZSTD_isError(cBlockSize))
            return cBlockSize;
        RETURN_ERROR_IF(cBlockSize > dctx->fParams.blockSizeMax, corruption_detected, "Block Size Exceeds Maximum");
        dctx->expected = cBlockSize;
        dctx->bType = bp.blockType;
        dctx->rleSize = bp.origSize;
        if (cBlockSize) {
            dctx->stage = bp.lastBlock ? ZSTDds_decompressLastBlock : ZSTDds_decompressBlock;
            return 0;
        }
        /* empty block */
        if (bp.lastBlock) {
            if (dctx->fParams.checksumFlag) {
                dctx->expected = 4;
                dctx->stage = ZSTDds_checkChecksum;
            } else {
                dctx->expected = 0; /* end of frame */
                dctx->stage = ZSTDds_getFrameHeaderSize;
            }
        } else {
            dctx->expected = ZSTD_blockHeaderSize; /* jump to next header */
            dctx->stage = ZSTDds_decodeBlockHeader;
        }
        return 0;
    }
}

/* Purpose: Decode one payload chunk and advance the frame after the complete block.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * dst borrows dstCapacity writable bytes and is used only by payload decoding.
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_decodeBlockPayload(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    DEBUGLOG(5, "ZSTD_decompressContinue: case ZSTDds_decompressBlock");
    {
        size_t rSize;
        switch (dctx->bType) {
        case bt_compressed:
            DEBUGLOG(5, "ZSTD_decompressContinue: case bt_compressed");
            assert(dctx->isFrameDecompression == 1);
            rSize = ZSTD_decompressBlock_internal(dctx, dst, dstCapacity, src, srcSize, is_streaming);
            dctx->expected = 0; /* Streaming not supported */
            break;
        case bt_raw:
            assert(srcSize <= dctx->expected);
            rSize = ZSTD_copyRawBlock(dst, dstCapacity, src, srcSize);
            FORWARD_IF_ERROR(rSize, "ZSTD_copyRawBlock failed");
            assert(rSize == srcSize);
            dctx->expected -= rSize;
            break;
        case bt_rle:
            rSize = ZSTD_setRleBlock(dst, dstCapacity, *(const BYTE*)src, dctx->rleSize);
            dctx->expected = 0; /* Streaming not supported */
            break;
        case bt_reserved: /* should never happen */
        default:
            RETURN_ERROR(corruption_detected, "invalid block type");
        }
        FORWARD_IF_ERROR(rSize, "");
        RETURN_ERROR_IF(rSize > dctx->fParams.blockSizeMax, corruption_detected,
                        "Decompressed Block Size Exceeds Maximum");
        DEBUGLOG(5, "ZSTD_decompressContinue: decoded size from block : %u", (unsigned)rSize);
        dctx->decodedSize += rSize;
        if (dctx->validateChecksum)
            XXH64_update(&dctx->xxhState, dst, rSize);
        dctx->previousDstEnd = (char*)dst + rSize;

        /* Stay on the same stage until we are finished streaming the block. */
        if (dctx->expected > 0) {
            return rSize;
        }

        if (dctx->stage == ZSTDds_decompressLastBlock) { /* end of frame */
            DEBUGLOG(4, "ZSTD_decompressContinue: decoded size from frame : %u", (unsigned)dctx->decodedSize);
            RETURN_ERROR_IF(dctx->fParams.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
                                dctx->decodedSize != dctx->fParams.frameContentSize,
                            corruption_detected, "");
            if (dctx->fParams.checksumFlag) { /* another round for frame checksum */
                dctx->expected = 4;
                dctx->stage = ZSTDds_checkChecksum;
            } else {
                ZSTD_DCtx_trace_end(dctx, dctx->decodedSize, dctx->processedCSize, /* streaming */ 1);
                dctx->expected = 0; /* ends here */
                dctx->stage = ZSTDds_getFrameHeaderSize;
            }
        } else {
            dctx->stage = ZSTDds_decodeBlockHeader;
            dctx->expected = ZSTD_blockHeaderSize;
        }
        return rSize;
    }
}

/* Purpose: Validate the frame checksum and complete tracing before resetting the stage.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_readFrameChecksum(ZSTD_DCtx* dctx, const void* src, size_t srcSize) {
    assert(srcSize == 4); /* guaranteed by dctx->expected */
    {
        if (dctx->validateChecksum) {
            U32 const h32 = (U32)XXH64_digest(&dctx->xxhState);
            U32 const check32 = MEM_readLE32(src);
            DEBUGLOG(4, "ZSTD_decompressContinue: checksum : calculated %08X :: %08X read", (unsigned)h32,
                     (unsigned)check32);
            RETURN_ERROR_IF(check32 != h32, checksum_wrong, "");
        }
        ZSTD_DCtx_trace_end(dctx, dctx->decodedSize, dctx->processedCSize, /* streaming */ 1);
        dctx->expected = 0;
        dctx->stage = ZSTDds_getFrameHeaderSize;
        return 0;
    }
}

/* Purpose: Complete a skippable header and record its payload extent.
 * Inputs: dctx owns decoder state; src borrows the exact stage-requested extent;
 * Outputs: Returns produced bytes or an error; preserves upstream state and consumption. */
static size_t ZSTD_readSkippableHeader(ZSTD_DCtx* dctx, const void* src, size_t srcSize) {
    assert(src != NULL);
    assert(srcSize <= ZSTD_SKIPPABLEHEADERSIZE);
    assert(dctx->format != ZSTD_f_zstd1_magicless);
    ZSTD_memcpy(dctx->headerBuffer + (ZSTD_SKIPPABLEHEADERSIZE - srcSize), src,
                srcSize); /* complete skippable header */
    dctx->expected =
        MEM_readLE32(dctx->headerBuffer +
                     ZSTD_FRAMEIDSIZE); /* note : dctx->expected can grow seriously large, beyond local buffer size */
    dctx->stage = ZSTDds_skipFrame;
    return 0;
}

/* Purpose: Consume one exact bufferless decoding stage and route its operation.
 * Inputs: dctx owns state; src borrows srcSize readable bytes; dst borrows dstCapacity writable bytes.
 * Outputs: Returns produced bytes or an error and publishes the next required input size. */
size_t ZSTD_decompressContinue(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    DEBUGLOG(5, "ZSTD_decompressContinue (srcSize:%u)", (unsigned)srcSize);
    RETURN_ERROR_IF(srcSize != ZSTD_nextSrcSizeToDecompressWithInputSize(dctx, srcSize), srcSize_wrong, "not allowed");
    ZSTD_checkContinuity(dctx, dst, dstCapacity);
    dctx->processedCSize += srcSize;
    switch (dctx->stage) {
    case ZSTDds_getFrameHeaderSize:
        return ZSTD_readFrameHeaderPrefix(dctx, src, srcSize);
    case ZSTDds_decodeFrameHeader:
        return ZSTD_readFrameHeaderSuffix(dctx, src, srcSize);
    case ZSTDds_decodeBlockHeader:
        return ZSTD_readBlockHeader(dctx, src);
    case ZSTDds_decompressLastBlock:
    case ZSTDds_decompressBlock:
        return ZSTD_decodeBlockPayload(dctx, dst, dstCapacity, src, srcSize);
    case ZSTDds_checkChecksum:
        return ZSTD_readFrameChecksum(dctx, src, srcSize);
    case ZSTDds_decodeSkippableHeader:
        return ZSTD_readSkippableHeader(dctx, src, srcSize);
    case ZSTDds_skipFrame:
        dctx->expected = 0;
        dctx->stage = ZSTDds_getFrameHeaderSize;
        return 0;
    default:
        assert(0);
        RETURN_ERROR(GENERIC, "impossible to reach");
    }
}
