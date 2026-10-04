/* Complete decoder ownership; the enclosing source retains upstream notices. */

#if SUPERZIP_LEGACY_DECODER_VERSION == 7
/* Purpose: Construct the decoder and history under the original custom allocator.
 * Inputs: Two callbacks or two null callbacks, with the upstream aligned nonthrowing contract.
 * Outputs: A fully initialized owner or NULL after matching rollback. */
ZSTDvXX_DCtx* ZSTDvXX_createDCtx_advanced(ZSTDvXX_customMem customMem) {
    ZBUFF_decoderOwner* owner;
    ZSTDvXX_DCtx* dctx;
    ZBUFF_bufferAllocator allocator;
    if (!customMem.customAlloc && !customMem.customFree)
        customMem = defaultCustomMem;
    allocator.allocate = customMem.customAlloc;
    allocator.release = customMem.customFree;
    allocator.opaque = customMem.opaque;
    owner = ZBUFF_createDecoderOwner(allocator, sizeof(ZSTDvXX_DCtx), _Alignof(ZSTDvXX_DCtx));
    if (owner == NULL)
        return NULL;
    dctx = (ZSTDvXX_DCtx*)ZBUFF_decoderStorage(owner);
    *dctx = (ZSTDvXX_DCtx){0};
    dctx->owner = owner;
    dctx->history = ZBUFF_decoderHistory(owner);
    dctx->customMem = customMem;
    if (ZSTDvXX_isError(ZSTDvXX_decompressBegin(dctx))) {
        ZBUFF_releaseDecoderOwner(owner);
        return NULL;
    }
    return dctx;
}
#endif

/* Purpose: Construct a complete default decoder ownership tree.
 * Inputs: None. Outputs: A fully initialized context or NULL after matching rollback. */
ZSTDvXX_DCtx* ZSTDvXX_createDCtx(void) {
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    return ZSTDvXX_createDCtx_advanced(defaultCustomMem);
#else
    ZBUFF_bufferAllocator const allocator = {NULL, NULL, NULL};
    ZBUFF_decoderOwner* const owner = ZBUFF_createDecoderOwner(allocator, sizeof(ZSTDvXX_DCtx), _Alignof(ZSTDvXX_DCtx));
    ZSTDvXX_DCtx* dctx;
    if (owner == NULL)
        return NULL;
    dctx = (ZSTDvXX_DCtx*)ZBUFF_decoderStorage(owner);
    *dctx = (ZSTDvXX_DCtx){0};
    dctx->owner = owner;
    dctx->history = ZBUFF_decoderHistory(owner);
    if (ZSTDvXX_isError(ZSTDvXX_decompressBegin(dctx))) {
        ZBUFF_releaseDecoderOwner(owner);
        return NULL;
    }
    return dctx;
#endif
}

/* Purpose: Release the whole decoder tree through its original allocation contract.
 * Inputs: One decoder or NULL. Outputs: Exactly matching history, storage and parent release. */
size_t ZSTDvXX_freeDCtx(ZSTDvXX_DCtx* dctx) {
    if (dctx != NULL)
        ZBUFF_releaseDecoderOwner(dctx->owner);
    return 0;
}

/* Purpose: Copy complete initialized decoder state and independently owned history.
 * Inputs: Two live contexts; destination ownership and custom allocator remain unchanged.
 * Outputs: Cloned state, or a recorded allocation error that every decoding entry point must return. */
void ZSTDvXX_copyDCtx(ZSTDvXX_DCtx* destination, const ZSTDvXX_DCtx* source) {
    ZBUFF_decoderOwner* owner;
    ZBUFF_ownedHistory* history;
    ZBUFF_bufferResult copied;
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    ZSTDvXX_customMem allocator;
#endif
    if (destination == source)
        return;
    owner = destination->owner;
    history = destination->history;
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    allocator = destination->customMem;
#endif
    copied = ZBUFF_cloneOwnedHistory(history, source->history);
    if (copied != ZBUFF_buffer_ok) {
        destination->historyError =
            copied == ZBUFF_buffer_allocation_failure ? ERROR(memory_allocation) : ERROR(GENERIC);
        return;
    }
    *destination = *source;
    destination->owner = owner;
    destination->history = history;
#if SUPERZIP_LEGACY_DECODER_VERSION == 7
    destination->customMem = allocator;
#endif
    destination->litPtr = destination->litBuffer;
}

/* Purpose: Capture successful bufferless output in bounded independent history.
 * Inputs: Live context and complete initialized output extent, never retained as borrowed storage.
 * Outputs: Status; errors are recorded until explicit session reset. */
static size_t ZSTDvXX_captureHistory(ZSTDvXX_DCtx* dctx, const void* output, size_t capacity, size_t bytes) {
    ZBUFF_bufferResult const captured = ZBUFF_appendOwnedHistory(dctx->history, output, capacity, 0, bytes);
    if (captured != ZBUFF_buffer_ok)
        dctx->historyError = captured == ZBUFF_buffer_allocation_failure ? ERROR(memory_allocation) : ERROR(GENERIC);
    return dctx->historyError;
}

/* Purpose: Measure a validated local output remainder without subtracting nullable empty pointers.
 * Inputs: End/cursor belong to one complete output extent, or both are null for an empty extent.
 * Outputs: Remaining bytes; no pointer is formed or retained. */
static size_t ZSTDvXX_remaining(const BYTE* end, const BYTE* cursor) {
    return cursor == NULL ? 0 : (size_t)(end - cursor);
}
