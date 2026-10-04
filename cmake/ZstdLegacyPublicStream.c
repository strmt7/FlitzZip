/* Purpose: Route validated public buffers to their initialized legacy decoder.
 * Inputs: legacyContext owns version state; input/output borrow live extents or null empty buffers.
 * Outputs: Updates byte positions and returns the native hint/error without replacing public buffer identities. */
MEM_STATIC size_t ZSTD_decompressLegacyStream(void* legacyContext, U32 version,
                                              ZSTD_outBuffer* output, ZSTD_inBuffer* input)
{
#if (ZSTD_LEGACY_SUPPORT <= 4)
    /* Optional v0.4 retains its historical null-buffer adaptation. SuperZip ships v0.5 and later. */
    static char x;
    /* Avoid passing NULL to legacy decoding. */
    if (output->dst == NULL) {
        assert(output->size == 0);
        output->dst = &x;
    }
    if (input->src == NULL) {
        assert(input->size == 0);
        input->src = &x;
    }
#endif
    DEBUGLOG(5, "ZSTD_decompressLegacyStream for v0.%u", version);
    switch(version)
    {
        default :
        case 1 :
        case 2 :
        case 3 :
            (void)legacyContext; (void)output; (void)input;
            return ERROR(version_unsupported);
#if (ZSTD_LEGACY_SUPPORT <= 4)
        case 4 :
            {
                ZBUFFv04_DCtx* dctx = (ZBUFFv04_DCtx*) legacyContext;
                const void* src = input->pos != 0 ? (const char*)input->src + input->pos : input->src;
                size_t readSize = input->size - input->pos;
                void* dst = output->pos != 0 ? (char*)output->dst + output->pos : output->dst;
                size_t decodedSize = output->size - output->pos;
                size_t const hintSize = ZBUFFv04_decompressContinue(dctx, dst, &decodedSize, src, &readSize);
                output->pos += decodedSize;
                input->pos += readSize;
                return hintSize;
            }
#endif
#if (ZSTD_LEGACY_SUPPORT <= 5)
        case 5 :
            {
                ZBUFFv05_DCtx* dctx = (ZBUFFv05_DCtx*) legacyContext;
                const void* src = input->pos != 0 ? (const char*)input->src + input->pos : input->src;
                size_t readSize = input->size - input->pos;
                void* dst = output->pos != 0 ? (char*)output->dst + output->pos : output->dst;
                size_t decodedSize = output->size - output->pos;
                size_t const hintSize = ZBUFFv05_decompressContinue(dctx, dst, &decodedSize, src, &readSize);
                output->pos += decodedSize;
                input->pos += readSize;
                return hintSize;
            }
#endif
#if (ZSTD_LEGACY_SUPPORT <= 6)
        case 6 :
            {
                ZBUFFv06_DCtx* dctx = (ZBUFFv06_DCtx*) legacyContext;
                const void* src = input->pos != 0 ? (const char*)input->src + input->pos : input->src;
                size_t readSize = input->size - input->pos;
                void* dst = output->pos != 0 ? (char*)output->dst + output->pos : output->dst;
                size_t decodedSize = output->size - output->pos;
                size_t const hintSize = ZBUFFv06_decompressContinue(dctx, dst, &decodedSize, src, &readSize);
                output->pos += decodedSize;
                input->pos += readSize;
                return hintSize;
            }
#endif
#if (ZSTD_LEGACY_SUPPORT <= 7)
        case 7 :
            {
                ZBUFFv07_DCtx* dctx = (ZBUFFv07_DCtx*) legacyContext;
                const void* src = input->pos != 0 ? (const char*)input->src + input->pos : input->src;
                size_t readSize = input->size - input->pos;
                void* dst = output->pos != 0 ? (char*)output->dst + output->pos : output->dst;
                size_t decodedSize = output->size - output->pos;
                size_t const hintSize = ZBUFFv07_decompressContinue(dctx, dst, &decodedSize, src, &readSize);
                output->pos += decodedSize;
                input->pos += readSize;
                return hintSize;
            }
#endif
    }
}


#if defined (__cplusplus)
}
#endif
