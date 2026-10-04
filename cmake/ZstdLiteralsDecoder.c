/* This fragment restructures the pinned Zstandard 1.5.7 literal decoder.
 * Its enclosing generated source retains the upstream dual-license notice. */

typedef struct {
    size_t lhSize;
    size_t litSize;
    size_t litCSize;
    U32 singleStream;
} ZSTD_LiteralsHeader;

/* Purpose: Decode a compressed literal header without consuming its payload.
 * Inputs: istart borrows at least five readable bytes; header is writable.
 * Outputs: Stores header, decoded and encoded extents plus stream count. */
static void ZSTD_decodeCompressedLiteralsHeader(const BYTE* istart, ZSTD_LiteralsHeader* header) {
    U32 const code = (istart[0] >> 2) & 3;
    U32 const bits = MEM_readLE32(istart);
    header->singleStream = 0;
    switch (code) {
    case 0:
    case 1:
    default:
        header->singleStream = !code;
        header->lhSize = 3;
        header->litSize = (bits >> 4) & 0x3FF;
        header->litCSize = (bits >> 14) & 0x3FF;
        break;
    case 2:
        header->lhSize = 4;
        header->litSize = (bits >> 4) & 0x3FFF;
        header->litCSize = bits >> 18;
        break;
    case 3:
        header->lhSize = 5;
        header->litSize = (bits >> 4) & 0x3FFFF;
        header->litCSize = (bits >> 22) + ((size_t)istart[4] << 10);
        break;
    }
}

/* Purpose: Decode raw/RLE literal lengths with their distinct payload requirements.
 * Inputs: istart borrows srcSize >= 2 bytes; rle selects a required following value byte.
 * Outputs: Stores bounded header metadata and returns zero or a corruption error. */
static size_t ZSTD_decodePlainLiteralsHeader(const BYTE* istart, size_t srcSize, int rle, ZSTD_LiteralsHeader* header) {
    U32 const code = (istart[0] >> 2) & 3;
    switch (code) {
    case 0:
    case 2:
    default:
        header->lhSize = 1;
        header->litSize = istart[0] >> 3;
        break;
    case 1:
        header->lhSize = 2;
        if (rle) {
            RETURN_ERROR_IF(srcSize < 3, corruption_detected,
                            "srcSize >= MIN_CBLOCK_SIZE == 2; here we need lhSize+1 = 3");
        }
        header->litSize = MEM_readLE16(istart) >> 4;
        break;
    case 3:
        header->lhSize = 3;
        if (rle) {
            RETURN_ERROR_IF(srcSize < 4, corruption_detected,
                            "srcSize >= MIN_CBLOCK_SIZE == 2; here we need lhSize+1 = 4");
        } else {
            RETURN_ERROR_IF(srcSize < 3, corruption_detected,
                            "srcSize >= MIN_CBLOCK_SIZE == 2; here we need lhSize = 3");
        }
        header->litSize = MEM_readLE24(istart) >> 4;
        break;
    }
    return 0;
}

/* Purpose: Dispatch one Huffman payload using existing or newly decoded tables.
 * Inputs: dctx owns the allocated literal extent and workspace; header/source
 *         extents are validated; encoding selects compressed or repeat mode.
 * Outputs: Writes literals and returns the upstream Huffman success/error result. */
static size_t ZSTD_decodeHuffmanLiterals(ZSTD_DCtx* dctx, const BYTE* istart, const ZSTD_LiteralsHeader* header,
                                         SymbolEncodingType_e encoding) {
    int const flags =
        (ZSTD_DCtx_get_bmi2(dctx) ? HUF_flags_bmi2 : 0) | (dctx->disableHufAsm ? HUF_flags_disableAsm : 0);
    if (encoding == set_repeat) {
        if (header->singleStream) {
            return HUF_decompress1X_usingDTable(dctx->litBuffer, header->litSize, istart + header->lhSize,
                                                header->litCSize, dctx->HUFptr, flags);
        }
        assert(header->litSize >= MIN_LITERALS_FOR_4_STREAMS);
        return HUF_decompress4X_usingDTable(dctx->litBuffer, header->litSize, istart + header->lhSize, header->litCSize,
                                            dctx->HUFptr, flags);
    }
    if (header->singleStream) {
#if defined(HUF_FORCE_DECOMPRESS_X2)
        return HUF_decompress1X_DCtx_wksp(dctx->entropy.hufTable, dctx->litBuffer, header->litSize,
                                          istart + header->lhSize, header->litCSize, dctx->workspace,
                                          sizeof(dctx->workspace), flags);
#else
        return HUF_decompress1X1_DCtx_wksp(dctx->entropy.hufTable, dctx->litBuffer, header->litSize,
                                           istart + header->lhSize, header->litCSize, dctx->workspace,
                                           sizeof(dctx->workspace), flags);
#endif
    }
    return HUF_decompress4X_hufOnly_wksp(dctx->entropy.hufTable, dctx->litBuffer, header->litSize,
                                         istart + header->lhSize, header->litCSize, dctx->workspace,
                                         sizeof(dctx->workspace), flags);
}

/* Purpose: Validate and decode compressed/repeated literals with existing split-buffer semantics.
 * Inputs: dctx owns state; istart/srcSize borrow a block; dst/capacity bound
 *         output; streaming selects placement; blockSizeMax is the context limit.
 * Outputs: Returns consumed bytes or the original error, publishing literal state only on success. */
static size_t ZSTD_decodeCompressedLiterals(ZSTD_DCtx* dctx, const BYTE* istart, size_t srcSize, void* dst,
                                            size_t dstCapacity, streaming_operation streaming, size_t blockSizeMax,
                                            SymbolEncodingType_e encoding) {
    ZSTD_LiteralsHeader header;
    size_t hufSuccess;
    size_t const expectedWriteSize = MIN(blockSizeMax, dstCapacity);
    RETURN_ERROR_IF(srcSize < 5, corruption_detected,
                    "srcSize >= MIN_CBLOCK_SIZE == 2; here we need up to 5 for case 3");
    ZSTD_decodeCompressedLiteralsHeader(istart, &header);
    RETURN_ERROR_IF(header.litSize > 0 && dst == NULL, dstSize_tooSmall, "NULL not handled");
    RETURN_ERROR_IF(header.litSize > blockSizeMax, corruption_detected, "");
    if (!header.singleStream) {
        RETURN_ERROR_IF(header.litSize < MIN_LITERALS_FOR_4_STREAMS, literals_headerWrong,
                        "Not enough literals (%zu) for the 4-streams mode (min %u)", header.litSize,
                        MIN_LITERALS_FOR_4_STREAMS);
    }
    RETURN_ERROR_IF(header.litCSize + header.lhSize > srcSize, corruption_detected, "");
    RETURN_ERROR_IF(expectedWriteSize < header.litSize, dstSize_tooSmall, "");
    ZSTD_allocateLiteralsBuffer(dctx, dst, dstCapacity, header.litSize, streaming, expectedWriteSize, 0);
    if (dctx->ddictIsCold && header.litSize > 768) {
        PREFETCH_AREA(dctx->HUFptr, sizeof(dctx->entropy.hufTable));
    }
    hufSuccess = ZSTD_decodeHuffmanLiterals(dctx, istart, &header, encoding);
    if (dctx->litBufferLocation == ZSTD_split) {
        assert(header.litSize > ZSTD_LITBUFFEREXTRASIZE);
        ZSTD_memcpy(dctx->litExtraBuffer, dctx->litBufferEnd - ZSTD_LITBUFFEREXTRASIZE, ZSTD_LITBUFFEREXTRASIZE);
        ZSTD_memmove(dctx->litBuffer + ZSTD_LITBUFFEREXTRASIZE - WILDCOPY_OVERLENGTH, dctx->litBuffer,
                     header.litSize - ZSTD_LITBUFFEREXTRASIZE);
        dctx->litBuffer += ZSTD_LITBUFFEREXTRASIZE - WILDCOPY_OVERLENGTH;
        dctx->litBufferEnd -= WILDCOPY_OVERLENGTH;
        assert(dctx->litBufferEnd <= (BYTE*)dst + blockSizeMax);
    }
    RETURN_ERROR_IF(HUF_isError(hufSuccess), corruption_detected, "");
    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = header.litSize;
    dctx->litEntropy = 1;
    if (encoding == set_compressed) {
        dctx->HUFptr = dctx->entropy.hufTable;
    }
    return header.litCSize + header.lhSize;
}

/* Purpose: Decode raw literals or safely borrow their padded source extent.
 * Inputs: dctx owns state; istart/srcSize borrow a block; dst/capacity bound
 *         output; streaming and blockSizeMax select the existing placement policy.
 * Outputs: Publishes literal extent and returns consumed bytes, or propagates boundedness errors. */
static size_t ZSTD_decodeRawLiterals(ZSTD_DCtx* dctx, const BYTE* istart, size_t srcSize, void* dst, size_t dstCapacity,
                                     streaming_operation streaming, size_t blockSizeMax) {
    ZSTD_LiteralsHeader header;
    size_t const expectedWriteSize = MIN(blockSizeMax, dstCapacity);
    FORWARD_IF_ERROR(ZSTD_decodePlainLiteralsHeader(istart, srcSize, 0, &header), "");
    RETURN_ERROR_IF(header.litSize > 0 && dst == NULL, dstSize_tooSmall, "NULL not handled");
    RETURN_ERROR_IF(header.litSize > blockSizeMax, corruption_detected, "");
    RETURN_ERROR_IF(expectedWriteSize < header.litSize, dstSize_tooSmall, "");
    ZSTD_allocateLiteralsBuffer(dctx, dst, dstCapacity, header.litSize, streaming, expectedWriteSize, 1);
    if (header.lhSize + header.litSize + WILDCOPY_OVERLENGTH > srcSize) {
        RETURN_ERROR_IF(header.litSize + header.lhSize > srcSize, corruption_detected, "");
        if (dctx->litBufferLocation == ZSTD_split) {
            ZSTD_memcpy(dctx->litBuffer, istart + header.lhSize, header.litSize - ZSTD_LITBUFFEREXTRASIZE);
            ZSTD_memcpy(dctx->litExtraBuffer, istart + header.lhSize + header.litSize - ZSTD_LITBUFFEREXTRASIZE,
                        ZSTD_LITBUFFEREXTRASIZE);
        } else {
            ZSTD_memcpy(dctx->litBuffer, istart + header.lhSize, header.litSize);
        }
        dctx->litPtr = dctx->litBuffer;
        dctx->litSize = header.litSize;
        return header.lhSize + header.litSize;
    }
    dctx->litPtr = istart + header.lhSize;
    dctx->litSize = header.litSize;
    dctx->litBufferEnd = dctx->litPtr + header.litSize;
    dctx->litBufferLocation = ZSTD_not_in_dst;
    return header.lhSize + header.litSize;
}

/* Purpose: Expand RLE literals into the existing split or contiguous destination.
 * Inputs: dctx owns state; istart/srcSize borrow a block; dst/capacity bound
 *         output; streaming and blockSizeMax select the existing placement policy.
 * Outputs: Publishes literal extent and returns header plus one value byte, or an error. */
static size_t ZSTD_decodeRleLiterals(ZSTD_DCtx* dctx, const BYTE* istart, size_t srcSize, void* dst, size_t dstCapacity,
                                     streaming_operation streaming, size_t blockSizeMax) {
    ZSTD_LiteralsHeader header;
    size_t const expectedWriteSize = MIN(blockSizeMax, dstCapacity);
    FORWARD_IF_ERROR(ZSTD_decodePlainLiteralsHeader(istart, srcSize, 1, &header), "");
    RETURN_ERROR_IF(header.litSize > 0 && dst == NULL, dstSize_tooSmall, "NULL not handled");
    RETURN_ERROR_IF(header.litSize > blockSizeMax, corruption_detected, "");
    RETURN_ERROR_IF(expectedWriteSize < header.litSize, dstSize_tooSmall, "");
    ZSTD_allocateLiteralsBuffer(dctx, dst, dstCapacity, header.litSize, streaming, expectedWriteSize, 1);
    if (dctx->litBufferLocation == ZSTD_split) {
        ZSTD_memset(dctx->litBuffer, istart[header.lhSize], header.litSize - ZSTD_LITBUFFEREXTRASIZE);
        ZSTD_memset(dctx->litExtraBuffer, istart[header.lhSize], ZSTD_LITBUFFEREXTRASIZE);
    } else {
        ZSTD_memset(dctx->litBuffer, istart[header.lhSize], header.litSize);
    }
    dctx->litPtr = dctx->litBuffer;
    dctx->litSize = header.litSize;
    return header.lhSize + 1;
}

/* Purpose: Route a literal section without mixing four decoding state machines.
 * Inputs: dctx owns decoder state; src/srcSize borrow input; dst/capacity bound
 *         output; streaming preserves the caller's literal-placement contract.
 * Outputs: Returns consumed source bytes or the same upstream validation error. */
static size_t ZSTD_decodeLiteralsBlock(ZSTD_DCtx* dctx, const void* src, size_t srcSize, void* dst, size_t dstCapacity,
                                       const streaming_operation streaming) {
    const BYTE* const istart = (const BYTE*)src;
    size_t blockSizeMax;
    SymbolEncodingType_e encoding;
    DEBUGLOG(5, "ZSTD_decodeLiteralsBlock");
    RETURN_ERROR_IF(srcSize < MIN_CBLOCK_SIZE, corruption_detected, "");
    encoding = (SymbolEncodingType_e)(istart[0] & 3);
    blockSizeMax = ZSTD_blockSizeMax(dctx);
    switch (encoding) {
    case set_repeat:
        DEBUGLOG(5, "set_repeat flag : re-using stats from previous compressed literals block");
        RETURN_ERROR_IF(dctx->litEntropy == 0, dictionary_corrupted, "");
        ZSTD_FALLTHROUGH;
    case set_compressed:
        return ZSTD_decodeCompressedLiterals(dctx, istart, srcSize, dst, dstCapacity, streaming, blockSizeMax,
                                             encoding);
    case set_basic:
        return ZSTD_decodeRawLiterals(dctx, istart, srcSize, dst, dstCapacity, streaming, blockSizeMax);
    case set_rle:
        return ZSTD_decodeRleLiterals(dctx, istart, srcSize, dst, dstCapacity, streaming, blockSizeMax);
    default:
        RETURN_ERROR(corruption_detected, "impossible");
    }
}
