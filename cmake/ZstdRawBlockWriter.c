/* Purpose: Write a raw block only after establishing its complete output extent.
 * Inputs: src owns srcSize bytes when nonempty; dst owns dstCapacity bytes.
 * Outputs: Returns header plus payload size, or dstSize_tooSmall before any
 * access when the advertised destination cannot contain that extent. */
MEM_STATIC size_t
ZSTD_noCompressBlock(void* dst, size_t dstCapacity, const void* src, size_t srcSize, U32 lastBlock)
{
    U32 cBlockHeader24;
    DEBUGLOG(5, "ZSTD_noCompressBlock (srcSize=%zu, dstCapacity=%zu)", srcSize, dstCapacity);
    RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize,
                    dstSize_tooSmall, "dst buf too small for uncompressed block");
    cBlockHeader24 = lastBlock + (((U32)bt_raw)<<1) + (U32)(srcSize << 3);
    MEM_writeLE24(dst, cBlockHeader24);
    if (srcSize != 0) ZSTD_memcpy((BYTE*)dst + ZSTD_blockHeaderSize, src, srcSize);
    return ZSTD_blockHeaderSize + srcSize;
}
