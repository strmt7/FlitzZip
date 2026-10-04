/* Purpose: Select a dictionary using only initialized input suffixes.
 * Inputs: customDictContent borrows dictContentSize initialized bytes within
 *         dictBufferCapacity; builders validate samples, offsets and params.
 * Outputs: Returns an owned finalized dictionary or an error; all temporary
 *          allocations are released, and finalized sizes never bound input. */
COVER_dictSelection_t COVER_selectDict(BYTE* customDictContent, size_t dictBufferCapacity, size_t dictContentSize,
                                       const BYTE* samplesBuffer, const size_t* samplesSizes,
                                       unsigned nbFinalizeSamples, size_t nbCheckSamples, size_t nbSamples,
                                       ZDICT_cover_params_t params, size_t* offsets, size_t totalCompressedSize) {
    const size_t initializedContentSize = dictContentSize;
    const BYTE* const customDictContentEnd = customDictContent + initializedContentSize;
    BYTE* const largestDictBuffer = (BYTE*)malloc(dictBufferCapacity);
    BYTE* const candidateDictBuffer = (BYTE*)malloc(dictBufferCapacity);
    const double regressionTolerance = ((double)params.shrinkDictMaxRegression / 100.0) + 1.00;
    size_t largestDictSize;
    size_t largestCompressedSize;
    size_t candidateContentSize = ZDICT_DICTSIZE_MIN;

    if (!largestDictBuffer || !candidateDictBuffer) {
        free(largestDictBuffer);
        free(candidateDictBuffer);
        return COVER_dictSelectionError(ERROR(memory_allocation));
    }

    largestDictSize =
        ZDICT_finalizeDictionary(largestDictBuffer, dictBufferCapacity, customDictContent, initializedContentSize,
                                 samplesBuffer, samplesSizes, nbFinalizeSamples, params.zParams);
    if (ZDICT_isError(largestDictSize)) {
        free(largestDictBuffer);
        free(candidateDictBuffer);
        return COVER_dictSelectionError(largestDictSize);
    }
    largestCompressedSize = COVER_checkTotalCompressedSize(params, samplesSizes, samplesBuffer, offsets, nbCheckSamples,
                                                           nbSamples, largestDictBuffer, largestDictSize);
    if (ZSTD_isError(largestCompressedSize)) {
        free(largestDictBuffer);
        free(candidateDictBuffer);
        return COVER_dictSelectionError(largestCompressedSize);
    }
    if (params.shrinkDict == 0) {
        free(candidateDictBuffer);
        return setDictSelection(largestDictBuffer, largestDictSize, largestCompressedSize);
    }

    while (candidateContentSize < largestDictSize && candidateContentSize <= initializedContentSize) {
        const size_t candidateDictSize = ZDICT_finalizeDictionary(
            candidateDictBuffer, dictBufferCapacity, customDictContentEnd - candidateContentSize, candidateContentSize,
            samplesBuffer, samplesSizes, nbFinalizeSamples, params.zParams);
        if (ZDICT_isError(candidateDictSize)) {
            free(largestDictBuffer);
            free(candidateDictBuffer);
            return COVER_dictSelectionError(candidateDictSize);
        }
        totalCompressedSize =
            COVER_checkTotalCompressedSize(params, samplesSizes, samplesBuffer, offsets, nbCheckSamples, nbSamples,
                                           candidateDictBuffer, candidateDictSize);
        if (ZSTD_isError(totalCompressedSize)) {
            free(largestDictBuffer);
            free(candidateDictBuffer);
            return COVER_dictSelectionError(totalCompressedSize);
        }
        if ((double)totalCompressedSize <= (double)largestCompressedSize * regressionTolerance) {
            free(largestDictBuffer);
            return setDictSelection(candidateDictBuffer, candidateDictSize, totalCompressedSize);
        }
        /* Preserve valid upstream candidates without overflowing the next extent. */
        if (candidateDictSize > initializedContentSize / 2) {
            break;
        }
        candidateContentSize = candidateDictSize * 2;
    }
    free(candidateDictBuffer);
    return setDictSelection(largestDictBuffer, largestDictSize, largestCompressedSize);
}
