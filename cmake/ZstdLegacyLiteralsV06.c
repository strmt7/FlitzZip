/* Version-specific literal decoding from pinned Zstandard 1.5.7.
 * The enclosing source retains the upstream dual-license notice. */

/* Purpose: Decode huffman literals using the v06 wire contract.
 * Inputs: dctx is writable; istart borrows srcSize bytes, at least MIN_CBLOCK_SIZE.
 * Outputs: Returns bytes consumed or an error; publishes the same literal state as upstream. */
static size_t ZSTDv06_decodeHuffmanLiterals(ZSTDv06_DCtx* dctx, const BYTE* istart, size_t srcSize) {
    size_t litSize, litCSize, singleStream = 0;
    U32 lhSize = ((istart[0]) >> 4) & 3;
    if (srcSize < 5)
        return ERROR(corruption_detected); /* srcSize >= MIN_CBLOCK_SIZE == 3; here we need up to 5 for lhSize, + cSize
                                              (+nbSeq) */
    switch (lhSize) {
    case 0:
    case 1:
    default: /* note : default is impossible, since lhSize into [0..3] */
        /* 2 - 2 - 10 - 10 */
        lhSize = 3;
        singleStream = istart[0] & 16;
        litSize = ((istart[0] & 15) << 6) + (istart[1] >> 2);
        litCSize = ((istart[1] & 3) << 8) + istart[2];
        break;
    case 2:
        /* 2 - 2 - 14 - 14 */
        lhSize = 4;
        litSize = ((istart[0] & 15) << 10) + (istart[1] << 2) + (istart[2] >> 6);
        litCSize = ((istart[2] & 63) << 8) + istart[3];
        break;
    case 3:
        /* 2 - 2 - 18 - 18 */
        lhSize = 5;
        litSize = ((istart[0] & 15) << 14) + (istart[1] << 6) + (istart[2] >> 2);
        litCSize = ((istart[2] & 3) << 16) + (istart[3] << 8) + istart[4];
        break;
    }
    if (litSize > ZSTDv06_BLOCKSIZE_MAX)
        return ERROR(corruption_detected);
    if (litCSize + lhSize > srcSize)
        return ERROR(corruption_detected);

    if (HUFv06_isError(singleStream ? HUFv06_decompress1X2(dctx->litBuffer, litSize, istart + lhSize, litCSize)
                                    : HUFv06_decompress(dctx->litBuffer, litSize, istart + lhSize, litCSize)))
        return ERROR(corruption_detected);

    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = litSize;
    memset(dctx->litBuffer + dctx->litSize, 0, WILDCOPY_OVERLENGTH);
    return litCSize + lhSize;
}

/* Purpose: Decode repeat literals using the v06 wire contract.
 * Inputs: dctx is writable; istart borrows srcSize bytes, at least MIN_CBLOCK_SIZE.
 * Outputs: Returns bytes consumed or an error; publishes the same literal state as upstream. */
static size_t ZSTDv06_decodeRepeatLiterals(ZSTDv06_DCtx* dctx, const BYTE* istart, size_t srcSize) {
    size_t litSize, litCSize;
    U32 lhSize = ((istart[0]) >> 4) & 3;
    if (lhSize != 1) /* only case supported for now : small litSize, single stream */
        return ERROR(corruption_detected);
    if (!dctx->flagRepeatTable)
        return ERROR(dictionary_corrupted);

    /* 2 - 2 - 10 - 10 */
    lhSize = 3;
    litSize = ((istart[0] & 15) << 6) + (istart[1] >> 2);
    litCSize = ((istart[1] & 3) << 8) + istart[2];
    if (litCSize + lhSize > srcSize)
        return ERROR(corruption_detected);

    {
        size_t const errorCode =
            HUFv06_decompress1X4_usingDTable(dctx->litBuffer, litSize, istart + lhSize, litCSize, dctx->hufTableX4);
        if (HUFv06_isError(errorCode))
            return ERROR(corruption_detected);
    }
    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = litSize;
    memset(dctx->litBuffer + dctx->litSize, 0, WILDCOPY_OVERLENGTH);
    return litCSize + lhSize;
}

/* Purpose: Decode raw literals using the v06 wire contract.
 * Inputs: dctx is writable; istart borrows srcSize bytes, at least MIN_CBLOCK_SIZE.
 * Outputs: Returns bytes consumed or an error; publishes the same literal state as upstream. */
static size_t ZSTDv06_decodeRawLiterals(ZSTDv06_DCtx* dctx, const BYTE* istart, size_t srcSize) {
    size_t litSize;
    U32 lhSize = ((istart[0]) >> 4) & 3;
    switch (lhSize) {
    case 0:
    case 1:
    default: /* note : default is impossible, since lhSize into [0..3] */
        lhSize = 1;
        litSize = istart[0] & 31;
        break;
    case 2:
        litSize = ((istart[0] & 15) << 8) + istart[1];
        break;
    case 3:
        litSize = ((istart[0] & 15) << 16) + (istart[1] << 8) + istart[2];
        break;
    }

    if (lhSize + litSize + WILDCOPY_OVERLENGTH > srcSize) { /* risk reading beyond src buffer with wildcopy */
        if (litSize + lhSize > srcSize)
            return ERROR(corruption_detected);
        memcpy(dctx->litBuffer, istart + lhSize, litSize);
        dctx->litPtr = dctx->litBuffer;
        dctx->litSize = litSize;
        memset(dctx->litBuffer + dctx->litSize, 0, WILDCOPY_OVERLENGTH);
        return lhSize + litSize;
    }
    /* direct reference into compressed stream */
    dctx->litPtr = istart + lhSize;
    dctx->litSize = litSize;
    return lhSize + litSize;
}

/* Purpose: Decode rle literals using the v06 wire contract.
 * Inputs: dctx is writable; istart borrows srcSize bytes, at least MIN_CBLOCK_SIZE.
 * Outputs: Returns bytes consumed or an error; publishes the same literal state as upstream. */
static size_t ZSTDv06_decodeRleLiterals(ZSTDv06_DCtx* dctx, const BYTE* istart, size_t srcSize) {
    size_t litSize;
    U32 lhSize = ((istart[0]) >> 4) & 3;
    switch (lhSize) {
    case 0:
    case 1:
    default: /* note : default is impossible, since lhSize into [0..3] */
        lhSize = 1;
        litSize = istart[0] & 31;
        break;
    case 2:
        litSize = ((istart[0] & 15) << 8) + istart[1];
        break;
    case 3:
        litSize = ((istart[0] & 15) << 16) + (istart[1] << 8) + istart[2];
        if (srcSize < 4)
            return ERROR(corruption_detected); /* srcSize >= MIN_CBLOCK_SIZE == 3; here we need lhSize+1 = 4 */
        break;
    }
    if (litSize > ZSTDv06_BLOCKSIZE_MAX)
        return ERROR(corruption_detected);
    memset(dctx->litBuffer, istart[lhSize], litSize + WILDCOPY_OVERLENGTH);
    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = litSize;
    return lhSize + 1;
}

/* Purpose: Route a bounded literal segment to its version-specific decoder.
 * Inputs: dctx is writable; src borrows srcSize bytes within the compressed block.
 * Outputs: Returns consumed bytes or an error without changing ownership or format support. */
static size_t ZSTDv06_decodeLiteralsBlock(ZSTDv06_DCtx* dctx, const void* src, size_t srcSize) {
    const BYTE* const istart = (const BYTE*)src;
    if (srcSize < MIN_CBLOCK_SIZE)
        return ERROR(corruption_detected);
    switch (istart[0] >> 6) {
    case IS_HUF:
        return ZSTDv06_decodeHuffmanLiterals(dctx, istart, srcSize);
    case IS_PCH:
        return ZSTDv06_decodeRepeatLiterals(dctx, istart, srcSize);
    case IS_RAW:
        return ZSTDv06_decodeRawLiterals(dctx, istart, srcSize);
    case IS_RLE:
        return ZSTDv06_decodeRleLiterals(dctx, istart, srcSize);
    default:
        return ERROR(corruption_detected);
    }
}
