# Exact generated-source identities for bounded legacy history and sequence
# execution.

# Purpose: Replace one complete legacy implementation region in memory. Inputs:
# content owns a verified source revision; begin/end select ordered
# implementation boundaries and replacement is the reviewed complete region.
# Outputs: Returns proposed text through output or rejects missing boundaries.
function(superzip_zstd_replace_legacy_region content begin end replacement
         output)
  string(FIND "${content}" "${begin}" first)
  string(FIND "${content}" "${end}" last)
  if(first LESS 0 OR last LESS_EQUAL first)
    message(FATAL_ERROR "Zstandard legacy history boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${first} prefix)
  string(SUBSTRING "${content}" ${last} -1 suffix)
  string(CONCAT proposed "${prefix}" "${replacement}" "${suffix}")
  set(${output}
      "${proposed}"
      PARENT_SCOPE)
endfunction()

# Purpose: Replace fabricated legacy history addresses with bounded extents.
# Inputs: source_dir owns exact transactional stream revisions; three shipped
# versions retain their wire format and borrowed-storage lifetime contracts.
# Outputs: Publishes complete hash-bound revisions, rejects unknown input or
# output, and never changes upstream provenance archives.
function(superzip_patch_zstd_legacy_history source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_history_${version}")
    set(dictionary_key "_zstd_legacy_dictionary_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual_hash)
    superzip_zstd_patch_is_superseded("${actual_hash}" "${${key}_patched}"
                                      superseded)
    if(superseded
       OR actual_hash STREQUAL "${${key}_patched}"
       OR actual_hash STREQUAL "${${dictionary_key}_patched}")
      continue()
    endif()
    if(NOT actual_hash STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    if(version STREQUAL v07)
      set(dictionary_return "size_t")
      set(sequence_begin "static\nsize_t ZSTD${version}_execSequence(")
    else()
      set(dictionary_return "void")
      set(sequence_begin "static size_t ZSTD${version}_execSequence(")
    endif()
    superzip_zstd_replace_legacy_region(
      "${content}" "static void ZSTD${version}_checkContinuity("
      "static size_t ZSTD${version}_decompressBlock_internal(" "" content)
    superzip_zstd_replace_legacy_region(
      "${content}" "static ${dictionary_return} ZSTD${version}_refDictContent("
      "static size_t ZSTD${version}_loadEntropy(" "" content)
    string(TOUPPER "${version}" fragment_version)
    string(CONCAT fragment_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/"
                  "ZstdLegacyHistory${fragment_version}.c")
    file(READ "${fragment_path}" fragment)
    string(REGEX REPLACE "\n+$" "" fragment "${fragment}")
    superzip_zstd_replace_legacy_region(
      "${content}" "${sequence_begin}"
      "static size_t ZSTD${version}_decompressSequences(" "${fragment}\n\n"
      content)
    if(version STREQUAL v07)
      file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyBlockV07.c" block)
      string(REGEX REPLACE "\n+$" "" block "${block}")
      superzip_zstd_replace_legacy_region(
        "${content}" "\nsize_t ZSTDv07_decompressBlock("
        "static size_t ZSTDv07_generateNxBytes(" "\n${block}\n\n" content)
    endif()
    string(REPLACE "    const void* vBase;" "    size_t dictSize;" content
                   "${content}")
    string(REPLACE "    dctx->vBase = NULL;" "    dctx->dictSize = 0;" content
                   "${content}")
    string(REPLACE "    const BYTE* const vBase = (const BYTE*) (dctx->vBase);"
                   "    size_t const dictSize = dctx->dictSize;" content
                   "${content}")
    string(REPLACE "base, vBase, dictEnd" "base, dictSize, dictEnd" content
                   "${content}")
    superzip_write_verified_zstd_patch("${source}" "${actual_hash}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

set(_zstd_legacy_history_v05_original
    "2f500337550029ffc019fa7fda6564056c404a2acd9344ae373e1ab0a660dbe7")
set(_zstd_legacy_history_v05_patched
    "aad9e79a70142bd9c3dc16973c46c9bd669df883e43fc091cbb3137bdcaf5350")
set(_zstd_legacy_history_v06_original
    "6cda59e8a05d34eeb4bf40d10477f87e0b167ca2bce9dc0bdb0e09550f32a34b")
set(_zstd_legacy_history_v06_patched
    "9fb2717f459644e888259f03676b0a4bcd86d6dddce483e1d5737d44f1b464ac")
set(_zstd_legacy_history_v07_original
    "f7aadbe711bbb1f04d76c784ec6f26c7f911653fe1c526c17c9c11e959bb8bc4")
set(_zstd_legacy_history_v07_patched
    "57c8f43bf5b39908b40e6fe02a2108f99f5dc436644b98f347b754052e0407b1")
