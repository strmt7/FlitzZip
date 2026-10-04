/* Purpose: Decode one frame through a complete scoped decoder ownership tree.
 * Inputs: Complete readable input and writable output extents.
 * Outputs: Native status and matching release on every decode result. */
size_t ZSTDvXX_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    size_t result;
    ZSTDvXX_DCtx* const dctx = ZSTDvXX_createDCtx();
    if (dctx == NULL)
        return ERROR(memory_allocation);
    result = ZSTDvXX_decompressDCtx(dctx, dst, dstCapacity, src, srcSize);
    ZSTDvXX_freeDCtx(dctx);
    return result;
}
