/* Purpose: Publish block history only after successful legacy decompression.
 * Inputs: dctx is initialized; dst/src borrow their advertised writable/readable extents.
 * Outputs: Returns the decoder result; errors never become history pointers or advance its output end. */
size_t ZSTDv07_decompressBlock(ZSTDv07_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {
    size_t result;
    ZSTDv07_checkContinuity(dctx, dst);
    result = ZSTDv07_decompressBlock_internal(dctx, dst, dstCapacity, src, srcSize);
    if (ZSTDv07_isError(result))
        return result;
    dctx->previousDstEnd = result != 0 ? (BYTE*)dst + result : dst;
    return result;
}

/* Purpose: Borrow an uncompressed block as bounded legacy history.
 * Inputs: blockStart owns blockSize bytes; it remains live until decoder use ends.
 * Outputs: Returns blockSize and publishes its end without arithmetic on a null empty pointer. */
ZSTDLIBv07_API size_t ZSTDv07_insertBlock(ZSTDv07_DCtx* dctx, const void* blockStart, size_t blockSize) {
    ZSTDv07_checkContinuity(dctx, blockStart);
    dctx->previousDstEnd = blockSize != 0 ? (const BYTE*)blockStart + blockSize : blockStart;
    return blockSize;
}

#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Check error propagation and valid history after a malformed canonical block call.
 * Inputs: None; all decoder and byte storage belongs to this bounded serial probe.
 * Outputs: Returns one only when the malformed call fails and the history end remains the destination start. */
int sz_legacy_v07_block_error_history(void) {
    ZSTDv07_DCtx* context = ZSTDv07_createDCtx();
    BYTE destination[64], malformed[1] = {0};
    size_t result;
    int preserved;
    if (context == NULL)
        return 0;
    result = ZSTDv07_decompressBlock(context, destination, sizeof(destination), malformed, sizeof(malformed));
    preserved = ZSTDv07_isError(result) && context->previousDstEnd == destination;
    ZSTDv07_freeDCtx(context);
    return preserved;
}
#endif
