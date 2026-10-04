# Purpose: Recognize exact later revisions outside the legacy decoder pipeline.
# Inputs: Complete current and preceding source hashes. Outputs: Returns true
# only for a known paired transformation; unknown bytes remain rejected.
function(superzip_zstd_nonlegacy_patch_is_superseded actual_hash patched_hash
         output)
  set(recognized FALSE)
  # A later comment-only revision is also a complete result of its preceding
  # logical patch. Accept only its exact identity paired with that baseline.
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCommentClarifications.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCompressionStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDecompressionStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdSynchronizationPoint.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdRawBlockWriter.cmake")
  if(actual_hash STREQUAL _zstd_raw_block_patched AND patched_hash STREQUAL
                                                      _zstd_raw_block_original)
    set(recognized TRUE)
  endif()
  if(actual_hash STREQUAL _zstd_synchronization_patched
     AND patched_hash STREQUAL _zstd_synchronization_original)
    set(recognized TRUE)
  endif()
  if((actual_hash STREQUAL _zstd_decompression_stream_patched
      OR actual_hash STREQUAL _zstd_decompression_stream_prior
      OR actual_hash STREQUAL _zstd_decompression_stream_guarded)
     AND patched_hash STREQUAL _zstd_decompression_stream_original)
    set(recognized TRUE)
  endif()
  if((actual_hash STREQUAL _zstd_compression_stream_patched
      OR actual_hash STREQUAL _zstd_compression_stream_prior)
     AND (patched_hash STREQUAL _zstd_comment_4_original
          OR patched_hash STREQUAL _zstd_comment_4_prior
          OR patched_hash STREQUAL _zstd_comment_4_patched))
    set(recognized TRUE)
  endif()
  math(EXPR last_comment_file "${_zstd_comment_file_count} - 1")
  foreach(comment_file RANGE 0 ${last_comment_file} 1)
    set(comment_key "_zstd_comment_${comment_file}")
    if(patched_hash STREQUAL "${${comment_key}_original}"
       AND (actual_hash STREQUAL "${${comment_key}_patched}"
            OR actual_hash STREQUAL "${${comment_key}_prior}"))
      set(recognized TRUE)
    endif()
  endforeach()
  set(${output}
      "${recognized}"
      PARENT_SCOPE)
endfunction()

# Purpose: Recognize complete later revisions of an exact preceding patch.
# Inputs: actual_hash and patched_hash identify full generated-source bytes.
# Outputs: Returns true through output only for an explicitly linked pair;
# unknown revisions remain subject to the writer's source identity rejection.
function(superzip_zstd_patch_is_superseded actual_hash patched_hash output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedBuffers.cmake")
  superzip_zstd_owned_predecessor("${actual_hash}" predecessor)
  if(NOT predecessor STREQUAL "")
    superzip_zstd_patch_is_superseded("${predecessor}" "${patched_hash}"
                                      recognized)
    if(predecessor STREQUAL patched_hash)
      set(recognized TRUE)
    endif()
    set(${output}
        "${recognized}"
        PARENT_SCOPE)
    return()
  endif()
  set(recognized FALSE)
  # Later decoder refactors are complete revisions of their exact preceding
  # constructor/comment patches. Bind each accepted edge to both identities.
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDictionaryBounds.cmake")
  superzip_zstd_dictionary_patch_is_superseded("${actual_hash}"
                                               "${patched_hash}" recognized)
  if(actual_hash STREQUAL _zstd_legacy_public_patched
     AND patched_hash STREQUAL _zstd_legacy_public_original)
    set(recognized TRUE)
  endif()
  foreach(version IN ITEMS v05 v06 v07)
    set(literal_key "_zstd_legacy_literals_${version}")
    set(stream_key "_zstd_legacy_stream_${version}")
    set(history_key "_zstd_legacy_history_${version}")
    set(dictionary_key "_zstd_legacy_dictionary_${version}")
    if(actual_hash STREQUAL "${${dictionary_key}_patched}"
       AND (patched_hash STREQUAL "${${history_key}_patched}"
            OR patched_hash STREQUAL "${${literal_key}_patched}"))
      set(recognized TRUE)
    endif()
    if((actual_hash STREQUAL "${${history_key}_patched}"
        OR actual_hash STREQUAL "${${dictionary_key}_patched}"
        OR actual_hash STREQUAL "${${stream_key}_patched}"
        OR actual_hash STREQUAL "${${stream_key}_empty_prior}"
        OR actual_hash STREQUAL "${${stream_key}_prior}"
       )
       AND (patched_hash STREQUAL "${${stream_key}_original}"
            OR patched_hash STREQUAL "${${literal_key}_original}"
            OR patched_hash STREQUAL "${${literal_key}_preceding}"))
      set(recognized TRUE)
    endif()
    if((actual_hash STREQUAL "${${history_key}_patched}"
        OR actual_hash STREQUAL "${${dictionary_key}_patched}")
       AND patched_hash STREQUAL "${${stream_key}_patched}")
      set(recognized TRUE)
    endif()
    if(actual_hash STREQUAL "${${literal_key}_patched}"
       AND (patched_hash STREQUAL "${${literal_key}_original}"
            OR patched_hash STREQUAL "${${literal_key}_preceding}"))
      set(recognized TRUE)
    endif()
  endforeach()
  superzip_zstd_nonlegacy_patch_is_superseded("${actual_hash}"
                                              "${patched_hash}" nonlegacy)
  if(nonlegacy)
    set(recognized TRUE)
  endif()
  set(${output}
      "${recognized}"
      PARENT_SCOPE)
endfunction()

# Purpose: Apply a pinned dependency transformation only to known source bytes.
# Inputs: path is an extracted source, hashes identify original/patched bytes,
# and content is the complete proposed text. Upstream archives stay immutable.
# Outputs: Atomically replaces the original on an exact match; rejects drift or
# an interrupted/concurrent patch instead of silently overwriting it.
function(superzip_write_verified_zstd_patch path original_hash patched_hash
         content)
  file(SHA256 "${path}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  superzip_zstd_patch_is_superseded("${actual_hash}" "${patched_hash}"
                                    superseded)
  if(superseded)
    return()
  endif()
  if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${path}")
  endif()
  string(SHA256 proposed_hash "${content}")
  if(NOT proposed_hash STREQUAL patched_hash)
    message(FATAL_ERROR "Zstandard patch output identity mismatch: ${path}; "
                        "expected ${patched_hash}, observed ${proposed_hash}")
  endif()
  set(temporary "${path}.superzip-patch")
  set(raw "${temporary}.raw")
  if(EXISTS "${temporary}" OR EXISTS "${raw}")
    message(FATAL_ERROR "Incomplete or concurrent Zstandard patch: ${path}")
  endif()
  file(WRITE "${raw}" "${content}")
  configure_file("${raw}" "${temporary}" @ONLY NEWLINE_STYLE LF)
  file(SHA256 "${temporary}" written_hash)
  if(NOT written_hash STREQUAL patched_hash)
    message(FATAL_ERROR "Zstandard patch write identity mismatch: ${path}")
  endif()
  file(RENAME "${temporary}" "${path}" RESULT rename_result)
  if(NOT rename_result STREQUAL "0")
    message(FATAL_ERROR "Zstandard patch publication failed: ${rename_result}")
  endif()
  file(REMOVE "${raw}")
endfunction()

# Purpose: Separate the modern bufferless decoder's stage operations. Inputs:
# source_dir owns verified extracted v1.5.7 source; the fragment keeps stage
# bodies and consumption semantics. Outputs: Publishes the exact reviewed
# revision or rejects source drift; never changes the upstream archive.
function(superzip_patch_zstd_bufferless source_dir)
  set(source "${source_dir}/lib/decompress/zstd_decompress.c")
  set(original_hash
      "029580818b7e9cd38d9d07c63516b00ceaa943ffcfbd099fc5ccbe3628fa362f")
  set(patched_hash
      "8ef204750636b93311fe4f0e6e2db66990e399ae78fc837dcf5c6c4c0d636354")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  superzip_zstd_patch_is_superseded("${actual_hash}" "${patched_hash}"
                                    superseded)
  if(superseded)
    return()
  endif()
  if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "size_t ZSTD_decompressContinue(" begin)
  string(FIND "${content}" "\n\n\nstatic size_t ZSTD_refDictContent" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard bufferless decoder boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdBufferlessDecoder.c"
       fragment)
  string(REGEX REPLACE "\n$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Separate the buffered decoder's header, memory and cursor operations.
# Inputs: source_dir owns exact v1.5.7 source after the bufferless revision.
# Outputs: Publishes reviewed stage functions or rejects source/output drift;
# preserves frame support, allocator semantics and the provenance archive.
function(superzip_patch_zstd_decompression_stream source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDecompressionStream.cmake")
  set(source "${source_dir}/lib/decompress/zstd_decompress.c")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL _zstd_decompression_stream_patched)
    return()
  endif()
  if(NOT actual_hash STREQUAL _zstd_decompression_stream_original
     AND NOT actual_hash STREQUAL _zstd_decompression_stream_prior
     AND NOT actual_hash STREQUAL _zstd_decompression_stream_guarded)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}"
              "/* Calls ZSTD_decompressContinue() with the right parameters"
              begin)
  string(FIND "${content}" "\n\nsize_t ZSTD_decompressStream_simpleArgs" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard streaming decoder boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDecompressionStream.c"
       fragment)
  string(REGEX REPLACE "\n$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch(
    "${source}" "${actual_hash}" "${_zstd_decompression_stream_patched}"
    "${content}")
endfunction()

# Purpose: Separate buffered/stable input loading, block encoding and flushing.
# Inputs: source_dir owns verified extracted v1.5.7 source after its comment
# foundation. Outputs: Publishes exact reviewed compressor bytes or rejects
# drift; keeps the provenance archive and wire algorithms unchanged.
function(superzip_patch_zstd_compression_stream source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCompressionStream.cmake")
  set(source "${source_dir}/lib/compress/zstd_compress.c")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL _zstd_compression_stream_patched)
    return()
  endif()
  if(NOT actual_hash STREQUAL _zstd_compression_stream_original
     AND NOT actual_hash STREQUAL _zstd_compression_stream_prior)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  if(actual_hash STREQUAL _zstd_compression_stream_prior)
    string(FIND "${content}" "/* Streaming compressor stage refactor" begin)
  else()
    string(FIND "${content}" "/** ZSTD_compressStream_generic():" begin)
  endif()
  string(FIND "${content}" "\n\nstatic size_t ZSTD_nextInputSizeHint_MTorST"
              end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard streaming compressor boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCompressionStream.c"
       fragment)
  string(REGEX REPLACE "\n$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch(
    "${source}" "${actual_hash}" "${_zstd_compression_stream_patched}"
    "${content}")
endfunction()

# Purpose: Repair v0.5 partial construction without changing the decoder.
# Inputs: source_dir is the extracted v1.5.7 root from the verified archive.
# Outputs: Writes only the identity-checked generated v0.5 source.
function(superzip_patch_zstd_v05 source_dir)
  set(decoder "${source_dir}/lib/legacy/zstd_v05.c")
  file(SHA256 "${decoder}" current_hash)
  if(current_hash STREQUAL
     "dd60a43788f2a2150ae9ddf91a43ea34c6a03d6a420196e209f5aca2ffb217e2")
    return()
  endif()
  file(READ "${decoder}" decoder_content)
  if(current_hash STREQUAL
     "62170472e18505b3347e563cdb1eba439312bb53385663730ac58cd92f3c4678")
    string(
      CONCAT constructor_contract
             "/* Purpose: Construct a fully owned buffered decoder, never a "
             "partial context.\n"
             " * Inputs: None; allocator failure is a normal error path.\n"
             " * Outputs: Returns a complete owner or NULL after releasing "
             "partial allocations. */\n")
    string(
      REPLACE
        "ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
        "${constructor_contract}ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
        decoder_content "${decoder_content}")
    string(CONCAT checked_create "    zbc->zc = ZSTDv05_createDCtx();\n"
                  "    if (zbc->zc==NULL) {\n        free(zbc);\n"
                  "        return NULL;\n    }\n")
    string(REPLACE "    zbc->zc = ZSTDv05_createDCtx();\n" "${checked_create}"
                   decoder_content "${decoder_content}")
  endif()
  string(REPLACE [=[if (offsetCode | !litLength)]=]
                 [=[if (offsetCode != 0 || litLength == 0)]=] decoder_content
                 "${decoder_content}")
  string(
    CONCAT patch_fragment_1
           "/* Purpose: Decode one legacy sequence and maintain rep"
           "eat-offset history.\n"
           " * Inputs: seq receives output; seqState owns validated"
           " entropy states and borrows the bitstream.\n"
           " * Outputs: Writes lengths and offset and advances deco"
           "der state without changing wire semantics. */\n"
           "static void ZSTDv05_decodeSequence(")
  string(REPLACE [=[static void ZSTDv05_decodeSequence(]=]
                 "${patch_fragment_1}" decoder_content "${decoder_content}")
  file(SHA256 "${decoder}" current_hash)
  set(original_hash
      "62170472e18505b3347e563cdb1eba439312bb53385663730ac58cd92f3c4678")
  # Accept only the exact earlier constructor repair for incremental builds.
  if(current_hash STREQUAL
     "ee643222919c3a354da6f71d69f55c681ae0647a274384f2143afbe63dd5d9be")
    set(original_hash "${current_hash}")
  endif()
  superzip_write_verified_zstd_patch(
    "${decoder}" "${original_hash}"
    "dd60a43788f2a2150ae9ddf91a43ea34c6a03d6a420196e209f5aca2ffb217e2"
    "${decoder_content}")

endfunction()

# Purpose: Keep shipped legacy context transitions ownership-safe on error.
# Inputs: source_dir is the verified, extracted v1.5.7 source root. Outputs:
# Patches only the generated initializer header with pinned identity.
function(superzip_patch_zstd_initializer source_dir)
  set(header "${source_dir}/lib/legacy/zstd_legacy.h")
  file(READ "${header}" header_content)
  string(
    CONCAT initializer_contract
           "/* Purpose: Initialize a legacy decoder without abandoning its "
           "current owner on failure.\n"
           " * Inputs: legacyContext owns prevVersion; dict is borrowed for "
           "the decoder lifetime.\n"
           " * Outputs: Publishes newVersion only on success; returns "
           "allocation/dictionary errors. */\n")
  string(
    REPLACE "MEM_STATIC size_t ZSTD_initLegacyStream("
            "${initializer_contract}MEM_STATIC size_t ZSTD_initLegacyStream("
            header_content "${header_content}")
  set(release_call "ZSTD_freeLegacyStreamContext(*legacyContext, prevVersion);")
  string(CONCAT early_release "    if (prevVersion != newVersion) "
                "${release_call}\n")
  string(REPLACE "${early_release}" "" header_content "${header_content}")
  string(CONCAT release_existing
                "            if (prevVersion != newVersion) ${release_call}\n")
  string(
    CONCAT unsupported
           "            (void)dict; (void)dictSize;\n"
           "            if (prevVersion != newVersion) {\n"
           "                ${release_call}\n"
           "                *legacyContext = NULL;\n            }\n"
           "            return 0;")
  string(
    REPLACE "            (void)dict; (void)dictSize;\n            return 0;"
            "${unsupported}" header_content "${header_content}")
  set(v04_init
      "            ZBUFFv04_decompressWithDictionary(dctx, dict, dictSize);\n")
  string(REPLACE "${v04_init}" "${v04_init}${release_existing}" header_content
                 "${header_content}")
  foreach(version IN ITEMS 5 6 7)
    string(
      CONCAT create "            ZBUFFv0${version}_DCtx* dctx = "
             "(prevVersion != newVersion) ? ZBUFFv0${version}_createDCtx() : "
             "(ZBUFFv0${version}_DCtx*)*legacyContext;\n")
    string(REPLACE "${create}" "${create}            size_t initResult;\n"
                   header_content "${header_content}")
    string(CONCAT initialize "            ZBUFFv0${version}_"
                  "decompressInitDictionary(dctx, dict, dictSize);\n")
    string(
      CONCAT checked_initialize
             "            initResult = ZBUFFv0${version}_"
             "decompressInitDictionary(dctx, dict, dictSize);\n"
             "            if (ZBUFFv0${version}_isError(initResult)) {\n"
             "                if (prevVersion != newVersion) "
             "ZBUFFv0${version}_freeDCtx(dctx);\n"
             "                return initResult;\n            }\n"
             "${release_existing}")
    string(REPLACE "${initialize}" "${checked_initialize}" header_content
                   "${header_content}")
  endforeach()
  superzip_write_verified_zstd_patch(
    "${header}"
    "90ecc3816d28da0e0537c610fe99209fb9603b1ecdb764bd1228a5226f5c7484"
    "a582b26687409283a230ff99d53cf0ccefa945ec86cf35111d00a3600e8eb848"
    "${header_content}")
endfunction()

# Purpose: Repair custom allocation failure and guard the complete private
# allocation header, including its dependencies. Inputs: source_dir is the
# verified v1.5.7 source root. Outputs: Writes only the hash-checked production
# allocation helper; preserves upstream notices.
function(superzip_patch_zstd_custom_allocator source_dir)
  set(header "${source_dir}/lib/common/allocations.h")
  set(original_hash
      "6a718e8edaca112abdc0bdfa7de67edefaa44c8d9e514b79672ab58ab3a9118a")
  set(previous_hash
      "b1e3a6f3460a0558727860cd0ade078c2311cc474c6a4aafc161bdf938c296cd")
  set(patched_hash
      "93837d129e3f4e7e73169759c41796f4f1378d4d31b218e766cdf304f1c8efff")
  file(SHA256 "${header}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  file(READ "${header}" content)
  if(actual_hash STREQUAL previous_hash)
    set(original_hash "${previous_hash}")
  else()
    string(
      CONCAT
        contract
        "/* Purpose: Zero custom storage only after successful allocation.\n"
        " * Inputs: size is the requested extent; customMem supplies "
        "callbacks or the standard allocator.\n"
        " * Outputs: Returns zeroed owned storage or NULL without accessing "
        "failed allocation. */\n")
    string(CONCAT signature "MEM_STATIC void* ZSTD_customCalloc(size_t size, "
                  "ZSTD_customMem customMem)\n")
    string(REPLACE "${signature}" "${contract}${signature}" content
                   "${content}")
    string(CONCAT checked_clear "        if (ptr == NULL) {\n"
                  "            return NULL;\n        }\n"
                  "        ZSTD_memset(ptr, 0, size);\n")
    string(REPLACE "        ZSTD_memset(ptr, 0, size);\n" "${checked_clear}"
                   content "${content}")
  endif()
  set(guard "#ifndef ZSTD_ALLOCATIONS_H\n#define ZSTD_ALLOCATIONS_H\n\n")
  string(REPLACE "${guard}" "" content "${content}")
  string(REPLACE "#define ZSTD_DEPS_NEED_MALLOC\n"
                 "${guard}#define ZSTD_DEPS_NEED_MALLOC\n" content "${content}")
  superzip_write_verified_zstd_patch("${header}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Guard a private dependency header without changing its declarations.
# Inputs: header is an extracted source; guard/anchor identify the insertion;
# original_hash/patched_hash pin both complete identities. Outputs: Publishes
# verified generated source or rejects drift/interruption.
function(superzip_patch_zstd_header_guard header guard anchor original_hash
         patched_hash)
  file(READ "${header}" content)
  string(CONCAT guarded_anchor "#ifndef ${guard}\n#define ${guard}\n\n"
                "${anchor}")
  string(REPLACE "${anchor}" "${guarded_anchor}" content "${content}")
  string(APPEND content "\n#endif /* ${guard} */\n")
  superzip_write_verified_zstd_patch("${header}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Replace dictionary selection with explicit input/output extents.
# Inputs: content is the known generated source; output receives repaired text.
# Outputs: Replaces the complete helper or rejects a missing source boundary.
function(superzip_rewrite_zstd_cover_selection content output)
  string(FIND "${content}" "COVER_dictSelection_t COVER_selectDict(" begin)
  string(CONCAT contract_prefix "/* Purpose: Select a dictionary "
                "using only initialized input suffixes.")
  string(FIND "${content}" "${contract_prefix}" contract_begin)
  if(contract_begin GREATER_EQUAL 0 AND contract_begin LESS begin)
    set(begin "${contract_begin}")
  endif()
  string(FIND "${content}" "/**\n * Parameters for COVER_tryParameters()." end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard dictionary selection boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCoverSelection.c" selection)
  set(${output}
      "${prefix}${selection}\n${suffix}"
      PARENT_SCOPE)
endfunction()

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_cover_cleanup source_dir)
  set(source "${source_dir}/lib/dictBuilder/cover.c")
  file(SHA256 "${source}" current_hash)
  set(patched_hash
      "fe6ea20bb307208825aff536dc3a1467b9961cb6caede2f1cd4308813667d867")
  if(current_hash STREQUAL patched_hash)
    return()
  endif()
  superzip_zstd_patch_is_superseded("${current_hash}" "${patched_hash}"
                                    superseded)
  if(superseded)
    return()
  endif()
  set(original_hash
      "2419631b20b0f4867d0f3c178fc4a3006d05f58e8ce3ce6657133f85ed40d683")
  file(READ "${source}" content)
  if(current_hash STREQUAL
     "01cdcb0f00af494a327b9fe8763d82b1d1bbbc63cc824669d26362dfa199126c"
     OR current_hash STREQUAL
        "89a86e4217d306fd236bfb41cbdb7081e13082d88acc0f780a713df46da3b0da")
    superzip_rewrite_zstd_cover_selection("${content}" content)
    superzip_write_verified_zstd_patch("${source}" "${current_hash}"
                                       "${patched_hash}" "${content}")
    return()
  endif()
  string(CONCAT patch_fragment_2 "  if (map->data) {\n"
                "    free(map->data);\n" "  }")
  string(REPLACE "${patch_fragment_2}" [=[  free(map->data);]=] content
                 "${content}")
  string(CONCAT patch_fragment_3 "  if (dst) {\n" "    free(dst);\n" "  }")
  string(REPLACE "${patch_fragment_3}" [=[  free(dst);]=] content "${content}")
  string(CONCAT patch_fragment_4 "  if (best->dict) {\n"
                "    free(best->dict);\n" "  }")
  string(REPLACE "${patch_fragment_4}" [=[  free(best->dict);]=] content
                 "${content}")
  string(CONCAT patch_fragment_5 "        if (best->dict) {\n"
                "          free(best->dict);\n" "        }")
  string(REPLACE "${patch_fragment_5}" [=[        free(best->dict);]=] content
                 "${content}")
  string(
    CONCAT patch_fragment_6
           "/* Purpose: Release map-owned storage and reset the map"
           ".\n"
           " * Inputs: map points to an initialized map; its data m"
           "ay be NULL.\n"
           " * Outputs: Releases data and clears size and ownership"
           ". */\n"
           "static void COVER_map_destroy(")
  string(REPLACE [=[static void COVER_map_destroy(]=] "${patch_fragment_6}"
                 content "${content}")
  string(
    CONCAT patch_fragment_7
           "/* Purpose: Measure sample compression using the candid"
           "ate dictionary.\n"
           " * Inputs: parameters, sample sizes/bytes/offsets and d"
           "ictionary extents are validated by the caller.\n"
           " * Outputs: Returns total compressed size or an error; "
           "releases temporary storage. */\n"
           "size_t COVER_checkTotalCompressedSize(")
  string(REPLACE [=[size_t COVER_checkTotalCompressedSize(]=]
                 "${patch_fragment_7}" content "${content}")
  string(
    CONCAT patch_fragment_8
           "/* Purpose: Wait for jobs and release optimizer-owned r"
           "esources.\n"
           " * Inputs: best is NULL or initialized shared state wit"
           "h counted live jobs.\n"
           " * Outputs: Joins logical completion and releases dicti"
           "onary and synchronization state. */\n"
           "void COVER_best_destroy(")
  string(REPLACE [=[void COVER_best_destroy(]=] "${patch_fragment_8}" content
                 "${content}")
  string(
    CONCAT patch_fragment_9
           "/* Purpose: Record worker completion and update the bes"
           "t dictionary under lock.\n"
           " * Inputs: best is NULL or initialized state; parameter"
           "s and selection describe one completed job.\n"
           " * Outputs: Decrements live jobs, updates owned diction"
           "ary and signals completion. */\n"
           "void COVER_best_finish(")
  string(REPLACE [=[void COVER_best_finish(]=] "${patch_fragment_9}" content
                 "${content}")
  superzip_rewrite_zstd_cover_selection("${content}" content)
  superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_worker_condition source_dir)
  set(source "${source_dir}/lib/compress/zstdmt_compress.c")
  file(READ "${source}" content)
  string(CONCAT patch_fragment_10
                "if (!mtctx->factory | !mtctx->jobs | !mtctx->bufPool | "
                "!mtctx->cctxPool | !mtctx->seqPool | initError)")
  string(CONCAT patch_fragment_11
                "if (!mtctx->factory || !mtctx->jobs || !mtctx->bufPool "
                "|| !mtctx->cctxPool || !mtctx->seqPool || initError)")
  string(REPLACE "${patch_fragment_10}" "${patch_fragment_11}" content
                 "${content}")
  string(
    CONCAT patch_fragment_12
           "/* Purpose: Construct a complete multithreaded compress"
           "ion owner.\n"
           " * Inputs: nbWorkers is validated; cMem supplies alloca"
           "tion callbacks; pool is optional borrowed state.\n"
           " * Outputs: Returns a complete context or NULL after re"
           "leasing partial allocations. */\n"
           "MEM_STATIC ZSTDMT_CCtx* ZSTDMT_createCCtx_advanced_inte"
           "rnal(")
  string(
    REPLACE [=[MEM_STATIC ZSTDMT_CCtx* ZSTDMT_createCCtx_advanced_internal(]=]
            "${patch_fragment_12}" content "${content}")
  superzip_write_verified_zstd_patch(
    "${source}"
    "c83db699b4041bf4db89c5558db490dd295635f1eeefd60b643fbc862bbac7b5"
    "0e2910244eedc12f7728f59e232325e964c2fc546f87f4bf74881016c47cf21a"
    "${content}")
endfunction()

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_v04_condition source_dir)
  set(source "${source_dir}/lib/legacy/zstd_v04.c")
  file(READ "${source}" content)
  string(REPLACE [=[if (offsetCode | !litLength)]=]
                 [=[if (offsetCode != 0 || litLength == 0)]=] content
                 "${content}")
  string(
    CONCAT patch_fragment_13
           "/* Purpose: Decode one legacy sequence and maintain rep"
           "eat-offset history.\n"
           " * Inputs: seq receives output; seqState owns validated"
           " entropy states and borrows the bitstream.\n"
           " * Outputs: Writes lengths and offset and advances deco"
           "der state without changing wire semantics. */\n"
           "static void ZSTD_decodeSequence(")
  string(REPLACE [=[static void ZSTD_decodeSequence(]=] "${patch_fragment_13}"
                 content "${content}")
  superzip_write_verified_zstd_patch(
    "${source}"
    "a80d9591ff3fc387a05e8e075713af9ad34596dc5c8681666346a716013a6e14"
    "079df3416c7aa806e7e9bb3a2db8c4c65dbe938b0b7e261d7b66591e0e29b551"
    "${content}")
endfunction()

# Purpose: Clarify reviewed algorithm explanations without changing compiled
# tokens. Inputs: source_dir is the verified generated dependency root; included
# data binds complete source identities. Outputs: Applies exact comment-only
# repairs or rejects drift; upstream archives remain unchanged.
function(superzip_patch_zstd_comment_clarifications source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCommentClarifications.cmake")
  math(EXPR last_file "${_zstd_comment_file_count} - 1")
  foreach(file_index RANGE 0 ${last_file} 1)
    set(key "_zstd_comment_${file_index}")
    set(source "${source_dir}/${${key}_path}")
    file(READ "${source}" content)
    math(EXPR last_comment "${${key}_count} - 1")
    foreach(comment_index RANGE 0 ${last_comment} 1)
      string(REPLACE "${${key}_${comment_index}_old}"
                     "${${key}_${comment_index}_new}" content "${content}")
    endforeach()
    file(SHA256 "${source}" actual_hash)
    set(original_hash "${${key}_original}")
    if(actual_hash STREQUAL "${${key}_prior}")
      set(original_hash "${actual_hash}")
    endif()
    superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

# Purpose: Compile assertion-only helpers only when assertions can use them.
# Inputs: source_dir is the verified dependency root; both helper bodies stay
# unchanged. Outputs: Preserves debug checks and removes unused release
# definitions; rejects source drift.
function(superzip_patch_zstd_assertion_helpers source_dir)
  foreach(component IN ITEMS huf_compress zstd_lazy)
    set(source "${source_dir}/lib/compress/${component}.c")
    file(READ "${source}" content)
    if(component STREQUAL "huf_compress")
      string(CONCAT begin_marker
                    "/* Returns 0 if the huffNode array is not sorted by "
                    "descending count */")
      set(end_marker "/* Insertion sort by descending order */")
      set(original_hash
          "0be78471f5175e49bbacb898d8b107bb8f7e13adc84780e0625be5861f016086")
      set(patched_hash
          "dfe5f1056dc7c8b5eaa9ef9e4333b6e78d9b8712504c7f6557d0d5ff3650ceb3")
      string(
        CONCAT
          contract
          "/* Purpose: Check descending symbol frequency order "
          "for enabled assertions.\n"
          " * Inputs: huffNode contains maxSymbolValue1 initialized nodes.\n"
          " * Outputs: Returns nonzero exactly when their counts "
          "are nonincreasing. */\n")
    else()
      set(begin_marker "/* ZSTD_isAligned():")
      set(end_marker "/* ZSTD_row_prefetch():")
      set(original_hash
          "dd6ccf357165dc8cb574ea56a34ff1db57b7b31728b32103d0c6b72319fd45b1")
      set(patched_hash
          "8f44e994583aacf9ed95004614d7fada1f54f470473d52080fc10836ba3faffe")
      string(
        CONCAT contract
               "/* Purpose: Check row alignment for enabled assertions.\n"
               " * Inputs: ptr is a borrowed row address; align is a "
               "nonzero power of two.\n"
               " * Outputs: Returns nonzero exactly when the address "
               "satisfies that alignment. */\n")
    endif()
    file(SHA256 "${source}" actual_hash)
    if(actual_hash STREQUAL patched_hash)
      continue()
    endif()
    string(FIND "${content}" "${begin_marker}" begin)
    string(FIND "${content}" "${end_marker}" end)
    if(begin LESS 0 OR end LESS_EQUAL begin)
      message(FATAL_ERROR "Zstandard assertion helper boundary mismatch")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${content}" ${begin} ${length} previous)
    string(FIND "${previous}" "MEM_STATIC" body_begin)
    string(SUBSTRING "${previous}" ${body_begin} -1 body)
    string(STRIP "${body}" body)
    string(CONCAT replacement "#ifndef NDEBUG\n" "${contract}" "${body}"
                  "\n#endif\n\n")
    string(REPLACE "${previous}" "${replacement}" content "${content}")
    superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                       "${patched_hash}" "${content}")
  endforeach()
endfunction()

# Purpose: Separate literal decoding state machines in the pinned dependency.
# Inputs: source_dir is the verified extracted root; the fragment preserves
# header, ownership, placement and error semantics. Outputs: Publishes exact
# reviewed bytes or rejects drift; preserves the upstream provenance archive.
function(superzip_patch_zstd_literals source_dir)
  set(source "${source_dir}/lib/decompress/zstd_decompress_block.c")
  set(original_hash
      "9cb8bcb07aeb87e4717a9c8a86d20832dc525dde2c1b72efdaaab7c937f43b87")
  set(patched_hash
      "9bfbc4cd0e92c2f51c5caefc62adb07d538371f166858dcdaf99c2a10d145490")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "/*! ZSTD_decodeLiteralsBlock() :" begin)
  string(FIND "${content}" "/* Hidden declaration for fullbench */" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard literal decoder boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLiteralsDecoder.c" fragment)
  string(CONCAT content "${prefix}" "${fragment}" "\n" "${suffix}")
  superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Apply the ordered construction, header and quality repair foundation
# before the legacy decoder refactors. Inputs: source_dir is the extracted root
# of the verified upstream archive. Outputs: Produces accepted foundation
# revisions or fails closed; leaves provenance untouched.
function(superzip_patch_zstd_foundation source_dir)
  superzip_patch_zstd_v05("${source_dir}")
  superzip_patch_zstd_cover_cleanup("${source_dir}")
  superzip_patch_zstd_worker_condition("${source_dir}")
  superzip_patch_zstd_v04_condition("${source_dir}")
  superzip_patch_zstd_initializer("${source_dir}")
  superzip_patch_zstd_custom_allocator("${source_dir}")
  superzip_patch_zstd_header_guard(
    "${source_dir}/lib/dictBuilder/cover.h"
    ZSTD_COVER_H
    "#ifndef ZDICT_STATIC_LINKING_ONLY\n"
    "6e2906e7e5c486a5b7f479f60210ff67d068b2c58744ad6a936ba5a9f4a23755"
    "432323ee0a02dbfb49abb146137fc152142df20b4223e1a8a5f6f6303a8eeed5")
  superzip_patch_zstd_header_guard(
    "${source_dir}/lib/compress/hist.h"
    ZSTD_HIST_H
    "/* --- dependencies --- */\n"
    "9e3363e69d5fa35c1f6e8d2970c6c2453c06fd8a8ab3cc516a14c77d07cdea35"
    "982484a96936e53fcca4c179e522fcb253608c93c2301ca636a3b29e1128b099")
  superzip_patch_zstd_comment_clarifications("${source_dir}")
  superzip_patch_zstd_assertion_helpers("${source_dir}")
  superzip_patch_zstd_literals("${source_dir}")
  superzip_patch_zstd_bufferless("${source_dir}")
endfunction()

# Purpose: Split legacy literal-kind algorithms from their dispatch without
# changing wire bytes, errors or buffer ownership. Inputs: source_dir owns the
# verified extracted archive. Outputs: Publishes only the exact final source
# revisions; accepts those same revisions on subsequent configuration.
function(superzip_patch_zstd_legacy_literals source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_literals_${version}")
    set(stream_key "_zstd_legacy_stream_${version}")
    set(history_key "_zstd_legacy_history_${version}")
    set(dictionary_key "_zstd_legacy_dictionary_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual_hash)
    superzip_zstd_patch_is_superseded("${actual_hash}" "${${key}_patched}"
                                      superseded)
    if(superseded
       OR actual_hash STREQUAL "${${key}_patched}"
       OR actual_hash STREQUAL "${${dictionary_key}_patched}"
       OR actual_hash STREQUAL "${${history_key}_patched}"
       OR actual_hash STREQUAL "${${stream_key}_patched}"
       OR actual_hash STREQUAL "${${stream_key}_empty_prior}"
       OR actual_hash STREQUAL "${${stream_key}_prior}")
      continue()
    endif()
    if(NOT actual_hash STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    string(FIND "${content}" "${${key}_begin}" begin)
    string(FIND "${content}" "${${key}_end}" end)
    if(begin LESS 0 OR end LESS_EQUAL begin)
      message(FATAL_ERROR "Zstandard legacy literal decoder boundary mismatch")
    endif()
    string(SUBSTRING "${content}" 0 ${begin} prefix)
    string(SUBSTRING "${content}" ${end} -1 suffix)
    string(TOUPPER "${version}" fragment_version)
    string(CONCAT fragment_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/"
                  "ZstdLegacyLiterals${fragment_version}.c")
    file(READ "${fragment_path}" fragment)
    string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
    string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
    superzip_write_verified_zstd_patch("${source}" "${${key}_original}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

# Purpose: Publish transactional legacy buffer ownership and small explicit
# stream stages. Inputs: source_dir owns exact preceding literal-decoder
# revisions. Outputs: Preserves old buffers on acquisition failure and retains
# supported wire formats, consumption, hints and allocator identities.
function(superzip_patch_zstd_legacy_stream source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_stream_${version}")
    set(history_key "_zstd_legacy_history_${version}")
    set(dictionary_key "_zstd_legacy_dictionary_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual_hash)
    superzip_zstd_patch_is_superseded("${actual_hash}" "${${key}_patched}"
                                      superseded)
    if(superseded
       OR actual_hash STREQUAL "${${key}_patched}"
       OR actual_hash STREQUAL "${${history_key}_patched}"
       OR actual_hash STREQUAL "${${dictionary_key}_patched}")
      continue()
    endif()
    if(NOT actual_hash STREQUAL "${${key}_original}"
       AND NOT actual_hash STREQUAL "${${key}_prior}"
       AND NOT actual_hash STREQUAL "${${key}_empty_prior}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    if(actual_hash STREQUAL "${${key}_original}")
      string(FIND "${content}" "${${key}_begin}" begin)
    else()
      string(FIND "${content}" "/* Transactional buffers and explicit stream"
                  begin)
    endif()
    string(FIND "${content}" "${${key}_end}" end)
    if(begin LESS 0 OR end LESS_EQUAL begin)
      message(FATAL_ERROR "Zstandard legacy stream boundary mismatch")
    endif()
    string(SUBSTRING "${content}" 0 ${begin} prefix)
    string(SUBSTRING "${content}" ${end} -1 suffix)
    string(TOUPPER "${version}" fragment_version)
    file(
      READ
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream${fragment_version}.c"
      fragment)
    string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
    string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
    superzip_write_verified_zstd_patch("${source}" "${actual_hash}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

# Purpose: Preserve empty public buffer identity through shipped legacy
# decoders. Inputs: source_dir owns verified v1.5.7 source after the initializer
# repair. Outputs: Guards zero offsets and removes the unneeded sentinel
# adaptation in shipped versions; preserves optional v0.4 semantics and rejects
# source drift.
function(superzip_patch_zstd_legacy_public_stream source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  set(source "${source_dir}/lib/legacy/zstd_legacy.h")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL _zstd_legacy_public_patched)
    return()
  endif()
  if(NOT actual_hash STREQUAL _zstd_legacy_public_original)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "MEM_STATIC size_t ZSTD_decompressLegacyStream("
              begin)
  string(FIND "${content}" "\n\n#endif   /* ZSTD_LEGACY_H */" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard public legacy stream boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyPublicStream.c"
       fragment)
  string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch(
    "${source}" "${actual_hash}" "${_zstd_legacy_public_patched}" "${content}")
endfunction()

# Purpose: Separate dictionary candidate selection from the selected item's
# ranking. Inputs: source_dir owns verified extracted v1.5.7 source; the
# fragment keeps overlap arithmetic and stable savings ordering. Outputs:
# Publishes exact reviewed bytes or rejects drift, preserving the provenance
# archive.
function(superzip_patch_zstd_dictionary_merge source_dir)
  set(source "${source_dir}/lib/dictBuilder/zdict.c")
  set(original_hash
      "dbe57910c9d446bbf2195f31bdba5cffef2c80b93c0325e9df8c9ffa744b0aa3")
  set(patched_hash
      "cc4d13b0ec7b765addbcab3a373c176e96567018ce1394f8ef71ffd2fa7f8c20")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "/*! ZDICT_tryMerge() :" begin)
  string(FIND "${content}" "\n\nstatic void ZDICT_removeDictItem" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard dictionary merger boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDictionaryMerge.c" fragment)
  string(REGEX REPLACE "\n$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch("${source}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Separate synchronization window initialization from rolling cut
# selection. Inputs: source_dir owns exact v1.5.7 source after its
# worker-construction repair. Outputs: Publishes reviewed role-separated stages
# or rejects source/output drift, preserving cut decisions, buffer ownership and
# the upstream provenance archive.
function(superzip_patch_zstd_synchronization_point source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdSynchronizationPoint.cmake")
  set(source "${source_dir}/lib/compress/zstdmt_compress.c")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL _zstd_synchronization_patched)
    return()
  endif()
  if(NOT actual_hash STREQUAL _zstd_synchronization_original)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(
    CONCAT begin_marker
           "/**\n * Searches through the input for a synchronization point.")
  string(FIND "${content}" "${begin_marker}" begin)
  string(FIND "${content}" "\n\nsize_t ZSTDMT_nextInputSizeHint" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard synchronization search boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdSynchronizationPoint.c"
       fragment)
  string(REGEX REPLACE "\n$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch(
    "${source}" "${_zstd_synchronization_original}"
    "${_zstd_synchronization_patched}" "${content}")
endfunction()

# Purpose: Reject impossible raw-block extents before writing or copying.
# Inputs: source_dir owns the exact comment-clarified v1.5.7 header. Outputs:
# Publishes the complete reviewed helper or rejects source/output drift.
function(superzip_patch_zstd_raw_block_writer source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdRawBlockWriter.cmake")
  set(source "${source_dir}/lib/compress/zstd_compress_internal.h")
  file(SHA256 "${source}" actual_hash)
  if(actual_hash STREQUAL _zstd_raw_block_patched)
    return()
  endif()
  if(NOT actual_hash STREQUAL _zstd_raw_block_original)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "/* ZSTD_noCompressBlock() :" begin)
  string(FIND "${content}" "\n\nMEM_STATIC size_t\nZSTD_rleCompressBlock" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard raw block writer boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdRawBlockWriter.c" fragment)
  string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
  string(CONCAT content "${prefix}" "${fragment}" "${suffix}")
  superzip_write_verified_zstd_patch("${source}" "${actual_hash}"
                                     "${_zstd_raw_block_patched}" "${content}")
endfunction()

# Purpose: Apply the verified predecessor revisions in their established order.
# Inputs: Extracted pinned source. Outputs: A complete known predecessor for the
# ownership migration, with all earlier repairs and provenance checks.
function(superzip_patch_zstd_base source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDictionaryBounds.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedBuffers.cmake")
  superzip_patch_zstd_foundation("${source_dir}")
  superzip_patch_zstd_compression_stream("${source_dir}")
  superzip_patch_zstd_decompression_stream("${source_dir}")
  superzip_patch_zstd_dictionary_merge("${source_dir}")
  superzip_patch_zstd_synchronization_point("${source_dir}")
  superzip_patch_zstd_legacy_literals("${source_dir}")
  superzip_patch_zstd_legacy_stream("${source_dir}")
  superzip_patch_zstd_legacy_history("${source_dir}")
  superzip_patch_zstd_legacy_dictionary("${source_dir}")
  superzip_patch_zstd_legacy_public_stream("${source_dir}")
  superzip_patch_zstd_raw_block_writer("${source_dir}")
  superzip_patch_zstd_dictionary_bounds("${source_dir}")
endfunction()

# Purpose: Apply the complete identity-bound production dependency pipeline.
# Inputs: Extracted pinned sources. Outputs: Complete reviewed revisions or
# identity rejection; the canonical product never selects a partial pipeline.
function(superzip_patch_zstd_legacy source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedBuffers.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDictionaryEvaluation.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdTableGeometry.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCoverWorkGroup.cmake")
  superzip_patch_zstd_base("${source_dir}")
  superzip_patch_zstd_legacy_owned_buffers("${source_dir}")
  superzip_patch_zstd_dictionary_evaluation("${source_dir}")
  superzip_patch_zstd_table_geometry("${source_dir}")
  superzip_patch_zstd_work_group("${source_dir}")
endfunction()
