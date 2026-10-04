/* Purpose: Select a dictionary through the checked C++ ownership boundary.
 * Inputs: content describes one live initialized allocation suffix; samples
 *         remain borrowed and output capacity is independent.
 * Outputs: Returns one owned dictionary or an ordinary Zstandard error. */
COVER_dictSelection_t COVER_selectDict(COVER_dictContent_t content, size_t dictBufferCapacity,
                                       const BYTE* samplesBuffer, const size_t* samplesSizes,
                                       unsigned nbFinalizeSamples, size_t nbCheckSamples, size_t nbSamples,
                                       ZDICT_cover_params_t params, size_t* offsets, size_t totalCompressedSize) {
    return COVER_selectDictOwned(content, dictBufferCapacity, samplesBuffer, samplesSizes, nbFinalizeSamples,
                                 nbCheckSamples, nbSamples, params, offsets, totalCompressedSize);
}
