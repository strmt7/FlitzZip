# Purpose: Recognize a complete ownership revision's exact predecessor. Inputs:
# actual_hash identifies complete source bytes. Outputs: Returns only a known
# predecessor or an empty result; never accepts drift.
function(superzip_zstd_owned_predecessor actual_hash output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdDictionaryEvaluation.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdTableGeometry.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCoverWorkGroup.cmake")
  set(predecessor "")
  if(actual_hash STREQUAL _zstd_dictionary_evaluation_patched)
    set(predecessor "${_zstd_dictionary_evaluation_original}")
  endif()
  foreach(flavor IN ITEMS cover fastcover)
    set(key "_zstd_work_group_${flavor}")
    if(actual_hash STREQUAL "${${key}_patched}")
      set(predecessor "${${key}_original}")
    endif()
  endforeach()
  foreach(part IN ITEMS huffman legacy fse_header fse_builder fse_sequences)
    set(key "_zstd_table_geometry_${part}")
    if(actual_hash STREQUAL "${${key}_patched}")
      set(predecessor "${${key}_original}")
    endif()
  endforeach()
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_owned_${version}")
    if(actual_hash STREQUAL "${${key}_patched}")
      set(predecessor "${${key}_original}")
    endif()
  endforeach()
  set(${output}
      "${predecessor}"
      PARENT_SCOPE)
endfunction()

# Purpose: Replace the complete buffered stream region with checked stages.
# Inputs: Verified preceding source and version. Outputs: Proposed source only.
function(superzip_zstd_rewrite_owned_stream content version output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  string(TOUPPER "${version}" fragment_version)
  string(CONCAT fragment_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/"
                "ZstdLegacyOwnedStream${fragment_version}.c")
  file(READ "${fragment_path}" fragment)
  string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
  set(stream_key "_zstd_legacy_stream_${version}")
  superzip_zstd_replace_legacy_region(
    "${content}" "/* Transactional buffers and explicit stream"
    "${${stream_key}_end}" "${fragment}" content)
  # The old size-limited copy helper has no remaining caller. All transfers
  # enforce full source and destination geometry in the checked stages.
  string(
    REGEX
    REPLACE
      "(static|MEM_STATIC) size_t ZBUFF${version}_limitCopy\\([^}]*}[^}]*}" ""
      content "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Give each C context one exclusive C++ buffer ownership tree. Inputs:
# Verified predecessor, version and its original allocator contract. Outputs:
# Proposed source with matching construction/destruction and no independent
# capacities.
function(superzip_zstd_rewrite_owned_context content version output)
  if(version STREQUAL v05)
    set(context zbc)
  else()
    set(context zbd)
  endif()
  string(REGEX REPLACE "    char\\* +inBuff;\n    size_t inBuffSize;"
                       "    ZBUFF_ownedBuffers* buffers;" content "${content}")
  string(REGEX REPLACE "    char\\* +outBuff;\n    size_t outBuffSize;\n" ""
                       content "${content}")
  if(version STREQUAL v07)
    set(allocator
        "{customMem.customAlloc, customMem.customFree, customMem.opaque}")
    string(
      REPLACE
        "    memcpy(&zbd->customMem, &customMem, sizeof(ZSTDv07_customMem));"
        "    zbd->customMem = customMem;" content "${content}")
    string(
      CONCAT release
             "    if (zbd->inBuff) zbd->customMem.customFree("
             "zbd->customMem.opaque, zbd->inBuff);\n"
             "    if (zbd->outBuff) zbd->customMem.customFree("
             "zbd->customMem.opaque, zbd->outBuff);")
  else()
    set(allocator "{NULL, NULL, NULL}")
    string(CONCAT release "    free(${context}->inBuff);\n"
                  "    free(${context}->outBuff);")
  endif()
  string(REPLACE "${release}"
                 "    ZBUFF_releaseOwnedBuffers(${context}->buffers);" content
                 "${content}")
  set(stage "ZBUFFds_init")
  if(version STREQUAL v05)
    set(stage "ZBUFFv05ds_init")
  endif()
  string(
    CONCAT
      acquire
      "    {\n        ZBUFF_bufferAllocator const allocator = ${allocator};\n"
      "        ${context}->buffers = ZBUFF_createOwnedBuffers(allocator);\n"
      "        if (${context}->buffers == NULL) {\n"
      "            ZBUFF${version}_freeDCtx(${context});\n"
      "            return NULL;\n        }\n    }\n"
      "    ${context}->stage = ${stage};")
  string(CONCAT ready "    ${context}->stage = ${stage};\n"
                "    return ${context};")
  string(REPLACE "${ready}" "${acquire}\n    return ${context};" content
                 "${content}")
  string(
    CONCAT
      release_contract
      "/* Purpose: Release one complete decoder ownership tree.\n"
      " * Inputs: A live owner or NULL. Outputs: Matching release of child, "
      "buffers and context. */\nsize_t ZBUFF${version}_freeDCtx(")
  string(REPLACE "size_t ZBUFF${version}_freeDCtx(" "${release_contract}"
                 content "${content}")
  if(NOT version STREQUAL v05)
    foreach(signature IN ITEMS "(void)"
                               "_advanced(ZSTDv07_customMem customMem)")
      set(construct
          "ZBUFF${version}_DCtx* ZBUFF${version}_createDCtx${signature}\n{")
      string(
        CONCAT construct_contract
               "/* Purpose: Construct a complete decoder with exclusive buffer "
               "ownership.\n * Inputs: Existing version-specific "
               "allocator contract. "
               "Outputs: Owner or NULL after complete rollback. */\n"
               "${construct}")
      string(REPLACE "${construct}" "${construct_contract}" content
                     "${content}")
    endforeach()
  endif()
  string(CONCAT include "#include \"ZstdLegacyBuffers.h\"\n\n"
                "struct ZBUFF${version}_DCtx_s {")
  string(REPLACE "struct ZBUFF${version}_DCtx_s {" "${include}" content
                 "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish checked ownership for all shipped legacy buffered decoders.
# Inputs: Extracted exact dictionary revisions. Outputs: Atomic complete
# revisions or a source/output identity rejection; preserves immutable upstream
# archives.
function(superzip_patch_zstd_legacy_owned_buffers source_dir)
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_owned_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual_hash)
    if(actual_hash STREQUAL "${${key}_patched}")
      continue()
    endif()
    superzip_zstd_patch_is_superseded("${actual_hash}" "${${key}_patched}"
                                      superseded)
    if(superseded)
      continue()
    endif()
    if(NOT actual_hash STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    superzip_zstd_rewrite_owned_stream("${content}" "${version}" content)
    superzip_zstd_rewrite_owned_context("${content}" "${version}" content)
    superzip_write_verified_zstd_patch("${source}" "${actual_hash}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

set(_zstd_legacy_owned_v05_original
    "bd627a804a8fd98dcc580cef65b3f190cac2f01e6136a9d5f0cc485ff36d08aa")
set(_zstd_legacy_owned_v06_original
    "4d8f737c5613cc1abbee283b383e3b97768448f244282511fef502c2b774baed")
set(_zstd_legacy_owned_v07_original
    "2fe88d3ccb50c4c0cf26da2aff6554d67af968f2a40b930ba8ac212f12fe72c8")
set(_zstd_legacy_owned_v05_patched
    "10b1201cf5047a87f6d76ca6ba4909491065184b39ea7eb1298ec599af9b6b22")
set(_zstd_legacy_owned_v06_patched
    "704b561f891236887ef6eb5bbe7ddf36165bc9b7a6fe35f6beb8e76f319987d6")
set(_zstd_legacy_owned_v07_patched
    "c214c586b9f2dd031bb8f0857296c63f88000a26b77d454f3297c63a69f7da7b")
