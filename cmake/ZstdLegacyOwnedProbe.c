#ifdef SUPERZIP_ZSTD_BUFFER_PROBES
/* Purpose: Prepare and clone actual decoder state before external source storage expires.
 * Inputs: Complete dictionary/raw-literal extents and an optional v07 destination allocator.
 * Outputs: Independent destination or NULL, with complete rollback and source-context release. */
void* sz_legacy_vXX_prepare_owned(const void* dictionary, size_t dictionaryBytes, const void* literals,
                                  size_t literalBytes, ZBUFF_bufferAllocator allocator) {
    ZSTDvXX_DCtx* source = NULL;
    ZSTDvXX_DCtx* destination = NULL;
    size_t initialized;
    if (dictionaryBytes > PTRDIFF_MAX || (dictionary == NULL && dictionaryBytes != 0) ||
        literalBytes < MIN_CBLOCK_SIZE || literalBytes > PTRDIFF_MAX || literals == NULL)
        return NULL;
#if SUPERZIP_LEGACY_DECODER_VERSION != 7
    if (allocator.allocate != NULL || allocator.release != NULL)
        return NULL;
#endif
    source = ZSTDvXX_createDCtx();
    if (source == NULL)
        goto failure;
    initialized = ZSTDvXX_decompressBegin_usingDict(source, dictionary, dictionaryBytes);
    if (ZSTDvXX_isError(initialized) ||
        ZSTDvXX_isError(ZSTDvXX_decodeRawLiterals(source, (const BYTE*)literals, literalBytes)))
        goto failure;
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    {
        ZSTDvXX_customMem const memory = {allocator.allocate, allocator.release, allocator.opaque};
        destination = ZSTDvXX_createDCtx_advanced(memory);
    }
#else
    destination = ZSTDvXX_createDCtx();
#endif
    if (destination == NULL)
        goto failure;
    ZSTDvXX_copyDCtx(destination, source);
    if (destination->historyError || destination->litPtr != destination->litBuffer)
        goto failure;
    ZSTDvXX_freeDCtx(source);
    return destination;
failure:
    ZSTDvXX_freeDCtx(destination);
    ZSTDvXX_freeDCtx(source);
    return NULL;
}

/* Purpose: Read prepared production literals and history through the canonical sequence helper.
 * Inputs: Prepared exclusive decoder, complete initialized prefix/output extents and sequence geometry.
 * Outputs: Native bytes or error, with no alternate decoder or retained output address. */
size_t sz_legacy_vXX_read_owned(void* context, void* destination, size_t capacity, size_t prefix, size_t literalLength,
                                size_t matchLength, size_t offset) {
    ZSTDvXX_DCtx* const dctx = (ZSTDvXX_DCtx*)context;
    BYTE* const output = (BYTE*)destination;
    const BYTE* cursor = dctx->litPtr;
    seq_t sequence;
    if (dctx->historyError)
        return dctx->historyError;
    if (prefix > capacity || capacity > PTRDIFF_MAX || (output == NULL && capacity != 0))
        return ERROR(dstSize_tooSmall);
    sequence.litLength = literalLength;
    sequence.matchLength = matchLength;
    sequence.offset = offset;
    return ZSTDvXX_execSequence(prefix != 0 ? output + prefix : output, capacity != 0 ? output + capacity : output,
                                sequence, &cursor, dctx->litBuffer + dctx->litSize, output, dctx->history);
}

/* Purpose: Exercise clone allocation failure and mandatory error propagation before output.
 * Inputs: Prepared destination, complete input/output extents and test allocator failure scheduling.
 * Outputs: Native prepared-frame result; source ownership is released after the actual clone/decode call. */
size_t sz_legacy_vXX_clone_decode(void* context, void* destination, size_t capacity, const void* dictionary,
                                  size_t dictionaryBytes, const void* source, size_t sourceBytes) {
    ZSTDvXX_DCtx* const dctx = (ZSTDvXX_DCtx*)context;
    ZSTDvXX_DCtx* const prepared = ZSTDvXX_createDCtx();
    size_t result;
    if (prepared == NULL)
        return ERROR(memory_allocation);
    result = ZSTDvXX_decompressBegin_usingDict(prepared, dictionary, dictionaryBytes);
    if (!ZSTDvXX_isError(result))
        result = ZSTDvXX_decompress_usingPreparedDCtx(dctx, prepared, destination, capacity, source, sourceBytes);
    ZSTDvXX_freeDCtx(prepared);
    return result;
}
#endif
