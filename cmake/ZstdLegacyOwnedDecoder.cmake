# Complete legacy decoder ownership; immutable upstream archives remain
# unchanged.

# Purpose: Replace an exact implementation region after its verified starting
# boundary. Inputs: Known complete source, ordered signature/end markers and
# reviewed replacement. Outputs: Proposed text or a boundary error, without
# writing the compiled source cache.
function(superzip_zstd_replace_owned_region content begin end replacement
         output)
  string(FIND "${content}" "${begin}" first REVERSE)
  if(first LESS 0)
    message(FATAL_ERROR "Zstandard owned decoder starting boundary mismatch")
  endif()
  string(SUBSTRING "${content}" ${first} -1 tail)
  string(FIND "${tail}" "${end}" offset)
  if(offset LESS_EQUAL 0)
    message(FATAL_ERROR "Zstandard owned decoder ending boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${first} prefix)
  string(SUBSTRING "${tail}" ${offset} -1 suffix)
  set(${output}
      "${prefix}${replacement}${suffix}"
      PARENT_SCOPE)
endfunction()

# Purpose: Replace borrowed decoder fields and make prefix addresses local to
# the active call. Inputs: Complete component-replaced source and a shipped
# version. Outputs: Proposed ownership and prefix contracts without retaining
# caller addresses.
function(superzip_zstd_owned_decoder_geometry content version output)
  if(version STREQUAL v05)
    set(window_log 26)
  else()
    set(window_log 27)
  endif()
  string(CONCAT _owned_boundary_text_0
                "    const void* previousDstEnd;\n    const void* b"
                "ase;\n    size_t dictSize;\n    const void* dictEn" "d;")
  string(CONCAT _owned_boundary_text_1
                "    ZBUFF_decoderOwner* owner;\n    ZBUFF_ownedHis"
                "tory* history;\n    size_t historyError;")
  string(REPLACE "${_owned_boundary_text_0}" "${_owned_boundary_text_1}"
                 content "${content}")
  string(CONCAT _owned_boundary_text_2
                "    dctx->previousDstEnd = NULL;\n    dctx->base ="
                " NULL;\n    dctx->dictSize = 0;\n    dctx->dictEnd" " = NULL;")
  string(
    CONCAT _owned_boundary_text_3
           "    if (ZBUFF_resetOwnedHistory(dctx->history, (si"
           "ze_t)1 << (MEM_32bits() ? 25 : ${window_log})) != "
           "ZBUFF_buffer_ok)\n        return ERROR(GENERIC);\n"
           "    dctx->historyError = 0;\n    dctx->litPtr = dc"
           "tx->litBuffer;\n    dctx->litSize = 0;")
  string(REPLACE "${_owned_boundary_text_2}" "${_owned_boundary_text_3}"
                 content "${content}")
  string(REPLACE "    const BYTE* const base = (const BYTE*) (dctx->base);"
                 "    const BYTE* const base = prefixStart;" content
                 "${content}")
  string(REPLACE "    size_t const dictSize = dctx->dictSize;"
                 "    const ZBUFF_ownedHistory* const history = dctx->history;"
                 content "${content}")
  string(
    REPLACE "    const BYTE* const dictEnd = (const BYTE*) (dctx->dictEnd);" ""
            content "${content}")
  string(REPLACE "base, dictSize, dictEnd)" "base, history)" content
                 "${content}")
  foreach(name IN ITEMS decompressSequences decompressBlock_internal)
    if(name STREQUAL decompressSequences)
      set(size seqSize)
    else()
      set(size srcSize)
    endif()
    string(
      REGEX
      REPLACE "(static size_t ZSTD${version}_${name}\\([^;{]*size_t ${size})\\)"
              "\\1, const BYTE* prefixStart)" content "${content}")
  endforeach()
  string(CONCAT _owned_boundary_text_4
                "ZSTD${version}_decompressSequences(dctx, dst, dstC"
                "apacity, ip, srcSize, prefixStart)")
  string(
    REPLACE
      "ZSTD${version}_decompressSequences(dctx, dst, dstCapacity, ip, srcSize)"
      "${_owned_boundary_text_4}" content "${content}")
  string(CONCAT _owned_boundary_text_5
                "ZSTD${version}_decompressBlock_internal(dctx, op, "
                "oend-op, ip, cBlockSize)")
  string(CONCAT _owned_boundary_text_6
                "ZSTD${version}_decompressBlock_internal(dctx, op, "
                "oend-op, ip, cBlockSize, ostart)")
  string(REPLACE "${_owned_boundary_text_5}" "${_owned_boundary_text_6}"
                 content "${content}")
  string(CONCAT _owned_boundary_text_7
                "\n[ \t]*(if \\(dstCapacity\\) )?ZSTD${version}_che"
                "ckContinuity\\([^;]+;\n")
  string(REGEX REPLACE "${_owned_boundary_text_7}" "\n" content "${content}")
  string(REPLACE "context->previousDstEnd == destination"
                 "ZBUFF_ownedHistorySize(context->history) == 0" content
                 "${content}")
  if(NOT version STREQUAL v07)
    string(CONCAT _owned_boundary_text_8
                  "ZSTD${version}_refDictContent(dctx, dict, dictSize"
                  ");\n        return 0;")
    string(REPLACE "${_owned_boundary_text_8}"
                   "return ZSTD${version}_refDictContent(dctx, dict, dictSize);"
                   content "${content}")
    string(
      REPLACE
        "ZSTD${version}_refDictContent(dctx, dict, dictSize);\n    return 0;"
        "return ZSTD${version}_refDictContent(dctx, dict, dictSize);" content
        "${content}")
    string(REPLACE "return sizeof(ZSTD${version}_DCtx);"
                   "return ZBUFF_decoderBaseSize(sizeof(ZSTD${version}_DCtx));"
                   content "${content}")
  else()
    string(REPLACE "return sizeof(*dctx);"
                   "return ZBUFF_decoderOwnedSize(dctx->owner);" content
                   "${content}")
    string(REPLACE "return sizeof(ZSTDv07_DCtx);"
                   "return ZBUFF_decoderBaseSize(sizeof(ZSTDv07_DCtx));"
                   content "${content}")
  endif()
  set(${output}
      "#include \"ZstdLegacyBuffers.h\"\n${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Reject invalid complete geometry and failed history cloning before
# decoder output. Inputs: Known component-replaced source and historical wire
# version. Outputs: Guards before local pointer formation and explicit contracts
# on every altered function.
function(superzip_zstd_owned_decoder_guards content version output)
  if(version STREQUAL v05)
    set(frame decompress_continueDCtx)
    set(header decodeFrameHeader_Part2)
    set(window "(size_t)1 << zc->params.windowLog")
  elseif(version STREQUAL v06)
    set(frame decompressFrame)
    set(header decodeFrameHeader)
    set(window "(size_t)1 << zc->fParams.windowLog")
  else()
    set(frame decompressFrame)
    set(header decodeFrameHeader)
    set(window "dctx->fParams.windowSize")
  endif()
  foreach(name IN ITEMS decompressSequences decompressBlock_internal "${frame}")
    if(name STREQUAL decompressSequences)
      set(capacity maxDstSize)
      set(input seqStart)
      set(bytes seqSize)
    elseif(version STREQUAL v05 AND name STREQUAL "${frame}")
      set(capacity maxDstSize)
      set(input src)
      set(bytes srcSize)
    else()
      set(capacity dstCapacity)
      set(input src)
      set(bytes srcSize)
    endif()
    string(
      CONCAT guard
             "    if (dctx->historyError) return dctx->historyError;\n"
             "    if (${capacity} > PTRDIFF_MAX || "
             "(dst == NULL && ${capacity} != 0)) "
             "return ERROR(dstSize_tooSmall);\n"
             "    if (${bytes} > PTRDIFF_MAX || "
             "(${input} == NULL && ${bytes} != 0)) "
             "return ERROR(srcSize_wrong);\n")
    string(REGEX
           REPLACE "(\nstatic size_t ZSTD${version}_${name}\\([^;{]*\\)\n\\{)"
                   "\\1\n${guard}" content "${content}")
  endforeach()
  foreach(capacity IN ITEMS maxDstSize dstCapacity)
    foreach(spaces IN ITEMS "" " ")
      string(
        REPLACE
          "BYTE* const oend = ostart${spaces}+${spaces}${capacity};"
          "BYTE* const oend = ${capacity} != 0 ? ostart + ${capacity} : ostart;"
          content "${content}")
    endforeach()
  endforeach()
  string(REPLACE "return op-ostart;"
                 "return op == NULL ? 0 : (size_t)(op-ostart);" content
                 "${content}")
  superzip_zstd_owned_empty_geometry("${content}" "${version}" content)
  if(version STREQUAL v07)
    set(context dctx)
  else()
    set(context zc)
  endif()
  string(CONCAT _owned_boundary_text_10
                "(\nstatic size_t ZSTD${version}_${header}\\([^;{]*"
                "\\)\n\\{[^{}]*)(    return result;)")
  string(
    CONCAT _owned_boundary_text_11
           "\\1    if (result == 0 && ZBUFF_limitOwnedHistory("
           "${context}->history, ${window}) != ZBUFF_buffer_ok"
           ")\n        return ERROR(frameParameter_unsupported"
           ");\n\\2")
  string(REGEX REPLACE "${_owned_boundary_text_10}"
                       "${_owned_boundary_text_11}" content "${content}")
  foreach(name IN
          ITEMS decompressBegin decompressSequences decompressBlock_internal
                "${frame}" "${header}" decompress_usingPreparedDCtx)
    string(
      CONCAT _owned_boundary_text_12
             "\n/* Purpose: Preserve the historical wire operati"
             "on with bounded independent history.\n * Inputs: E"
             "xclusive live decoder and complete input/output ex"
             "tents under the original API.\n * Outputs: Native "
             "status without retaining caller storage; explicit "
             "begin resets pending history errors. */\\1")
    string(
      REGEX
      REPLACE "(\n(static )?size_t ZSTD${version}_${name}\\([^;{]*\\)\n\\{)"
              "${_owned_boundary_text_12}" content "${content}")
  endforeach()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Preserve nullable empty extents without pointer arithmetic on null.
# Inputs: Reviewed decoder source with complete entry geometry guards. Outputs:
# Local remainder and input endpoints that preserve ordinary nonempty semantics.
function(superzip_zstd_owned_empty_geometry content version output)
  string(REPLACE "oend-op" "ZSTD${version}_remaining(oend, op)" content
                 "${content}")
  string(REPLACE "ip + seqSize;" "seqSize != 0 ? ip + seqSize : ip;" content
                 "${content}")
  string(REPLACE "ip + srcSize;" "srcSize != 0 ? ip + srcSize : ip;" content
                 "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish complete decoder ownership for all shipped historical
# versions. Inputs: Exact previous canonical sources or exact complete owned
# results. Outputs: Atomic hash-verified sources; unknown bytes and immutable
# upstream archives remain untouched.
function(superzip_patch_zstd_legacy_owned_decoder source_dir)
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_owned_decoder_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${${key}_patched}")
      continue()
    endif()
    if(NOT actual STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    superzip_zstd_owned_decoder_components("${content}" "${version}" content)
    superzip_zstd_owned_decoder_geometry("${content}" "${version}" content)
    superzip_zstd_owned_decoder_guards("${content}" "${version}" content)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedProbe.c" probe)
    string(REPLACE vXX "${version}" probe "${probe}")
    string(SUBSTRING "${version}" 1 2 number)
    string(REGEX REPLACE "^0" "" number "${number}")
    string(
      CONCAT _owned_boundary_text_13
             "\n#define SUPERZIP_LEGACY_DECODER_VERSION "
             "${number}\n${probe}\n#undef SUPERZIP_LEGACY_DECODE" "R_VERSION\n")
    string(APPEND content "${_owned_boundary_text_13}")
    superzip_write_verified_zstd_patch("${source}" "${actual}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/ZstdLegacyOwnedDecoderHashes.cmake")

# Purpose: Instantiate the canonical ownership components for one historical
# wire version. Inputs: Known source and a shipped version. Outputs: Proposed
# complete implementation regions.
function(superzip_zstd_owned_decoder_components content version output)
  string(SUBSTRING "${version}" 1 2 number)
  string(REGEX REPLACE "^0" "" number "${number}")
  string(TOUPPER "${version}" upper)
  foreach(component IN ITEMS owner sequence literals continue block one_shot)
    if(component STREQUAL owner)
      set(file ZstdLegacyDecoderOwner.c)
    elseif(component STREQUAL sequence)
      set(file ZstdLegacyOwnedSequence.c)
    elseif(component STREQUAL literals)
      set(file ZstdLegacyOwnedLiterals.c)
    elseif(component STREQUAL continue)
      set(file "ZstdLegacyOwnedContinue${upper}.c")
    elseif(component STREQUAL block)
      set(file ZstdLegacyOwnedBlock.c)
    else()
      set(file ZstdLegacyOwnedOneShot.c)
    endif()
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${file}" ${component})
    string(REPLACE vXX "${version}" ${component} "${${component}}")
  endforeach()
  string(CONCAT owner "#define SUPERZIP_LEGACY_DECODER_VERSION ${number}\n"
                "${owner}\n#undef SUPERZIP_LEGACY_DECODER_VERSION\n")
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedDictionary.c"
       dictionary)
  string(REPLACE vXX "${version}" dictionary "${dictionary}")
  set(sequence "${sequence}\n\n${dictionary}\n\n")
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedHeader.c" header)
  string(REPLACE vXX "${version}" header "${header}")
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedDataBlock.c"
       data_block)
  string(REPLACE vXX "${version}" data_block "${data_block}")
  if(version STREQUAL v05)
    set(stage ZSTDv05ds_decodeBlockHeader)
  else()
    set(stage ZSTDds_decodeBlockHeader)
  endif()
  string(
    CONCAT continue
           "#define SUPERZIP_LEGACY_DECODER_VERSION ${number}\n"
           "#define SUPERZIP_LEGACY_BLOCK_STAGE ${stage}\n${header}\n"
           "${data_block}\n${continue}\n#undef SUPERZIP_LEGACY_BLOCK_STAGE\n"
           "#undef SUPERZIP_LEGACY_DECODER_VERSION\n")
  if(NOT version MATCHES "^v0[567]$")
    message(FATAL_ERROR "Unsupported owned legacy decoder version")
  endif()
  superzip_zstd_owned_component_regions("${content}" "${version}" content)
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Replace complete canonical implementation regions using ordered exact
# boundaries. Inputs: Admitted source, shipped version and instantiated
# parent-scope fragments. Outputs: Complete proposed source; full output
# identity is checked before publication.
function(superzip_zstd_owned_component_regions content version output)
  if(version STREQUAL v07)
    set(factory createDCtx_advanced)
    set(section "\n\n/*-************************")
    set(frame "\n\n/* Purpose: Borrow an uncompressed block")
  else()
    set(factory createDCtx)
    set(section "\n\n/* ************************")
    if(version STREQUAL v05)
      set(frame "\n\n\n/*! ZSTDv05_decompress_continueDCtx")
    else()
      set(section "\n\n/*-************************")
      set(frame "\n\n\n/*! ZSTDv06_decompressFrame()")
    endif()
  endif()
  superzip_zstd_replace_owned_region(
    "${content}" "\nZSTD${version}_DCtx* ZSTD${version}_${factory}"
    "${section}" "\n${owner}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "/* Purpose: Execute a legacy sequence after proving"
    "static size_t ZSTD${version}_decompressSequences(" "${sequence}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "\nstatic size_t ZSTD${version}_decodeRawLiterals("
    "\n\n/* Purpose: Decode rle literals" "\n${literals}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "\nsize_t ZSTD${version}_decompressContinue("
    "\n\n\nstatic size_t ZSTD${version}_loadEntropy(" "\n${continue}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "\nsize_t ZSTD${version}_decompressBlock(" "${frame}"
    "\n${block}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "\nsize_t ZSTD${version}_decompress("
    "\n\n/* ZSTD_errorFrameSizeInfoLegacy" "\n${one_shot}" content)
  if(version STREQUAL v07)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedInsertV07.c"
         insert)
    superzip_zstd_replace_owned_region(
      "${content}" "\nZSTDLIBv07_API size_t ZSTDv07_insertBlock("
      "\n\n#ifdef SUPERZIP_ZSTD_BUFFER_PROBES" "\n${insert}" content)
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()
