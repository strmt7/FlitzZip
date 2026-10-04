/* Purpose: Decode raw literals into the decoder's independent padded storage.
 * Inputs: A complete readable segment of at least MIN_CBLOCK_SIZE bytes and a live decoder.
 * Outputs: Consumed bytes or corruption_detected; no compressed-input address is retained. */
static size_t ZSTDvXX_decodeRawLiterals(ZSTDvXX_DCtx* dctx, const BYTE* istart, size_t srcSize) {
    size_t litSize;
    U32 lhSize = ((istart[0]) >> 4) & 3;
    switch (lhSize) {
    case 0:
    case 1:
    default:
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
    if (lhSize > srcSize || litSize > srcSize - lhSize || litSize > sizeof(dctx->litBuffer) - WILDCOPY_OVERLENGTH)
        return ERROR(corruption_detected);
    if (ZBUFF_copyBytes(dctx->litBuffer, sizeof(dctx->litBuffer), 0, istart, srcSize, lhSize, litSize) !=
        ZBUFF_buffer_ok)
        return ERROR(corruption_detected);
    memset(dctx->litBuffer + litSize, 0, WILDCOPY_OVERLENGTH);
    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = litSize;
    return lhSize + litSize;
}
