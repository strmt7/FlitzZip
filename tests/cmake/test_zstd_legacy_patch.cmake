# Purpose: Verify reproducible dependency patching, idempotence and fail-closed
# source/write boundaries using isolated copies of the provenance archive.
# Inputs: REPO_ROOT and BINARY_DIR identify this checkout and its build tree.
# Outputs: Fails on a missing/incorrect transformation or unexpected overwrite;
# retains small generated fixtures in the build tree for diagnosis.
if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()
include("${REPO_ROOT}/cmake/PatchZstdLegacy.cmake")
if(DEFINED PATCH_SOURCE_DIR)
  superzip_patch_zstd_legacy("${PATCH_SOURCE_DIR}")
  return()
endif()
if(NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "BINARY_DIR is required")
endif()

set(ARCHIVE "${REPO_ROOT}/third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip")
file(SHA256 "${ARCHIVE}" archive_before)
if(NOT archive_before STREQUAL
   "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4")
  message(FATAL_ERROR "Dependency-patch fixture archive identity mismatch")
endif()
string(
  RANDOM
  LENGTH 16
  ALPHABET 0123456789abcdef nonce)
set(SCRATCH "${BINARY_DIR}/tests/zstd-legacy-patch-${nonce}")
if(EXISTS "${SCRATCH}")
  message(FATAL_ERROR "Refusing a preexisting dependency-patch fixture")
endif()
file(MAKE_DIRECTORY "${SCRATCH}")

# Purpose: Extract just the patched input files into a fresh fixture. Inputs:
# name identifies a test-owned directory below scratch. Outputs: Returns that
# fixture's source root through output; no binaries.
function(prepare_fixture name output)
  set(root "${SCRATCH}/${name}")
  file(MAKE_DIRECTORY "${root}")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -E tar xf "${ARCHIVE}" --
      "zstd-1.5.7/lib/legacy/zstd_v05.c" "zstd-1.5.7/lib/legacy/zstd_legacy.h"
      "zstd-1.5.7/lib/common/allocations.h" "zstd-1.5.7/lib/dictBuilder/cover.h"
      "zstd-1.5.7/lib/compress/hist.h" "zstd-1.5.7/lib/legacy/zstd_v04.c"
      "zstd-1.5.7/lib/compress/zstdmt_compress.c"
      "zstd-1.5.7/lib/dictBuilder/cover.c" "zstd-1.5.7/lib/dictBuilder/zdict.c"
      "zstd-1.5.7/lib/common/entropy_common.c" "zstd-1.5.7/lib/common/huf.h"
      "zstd-1.5.7/lib/common/xxhash.h" "zstd-1.5.7/lib/compress/fse_compress.c"
      "zstd-1.5.7/lib/compress/huf_compress.c"
      "zstd-1.5.7/lib/compress/zstd_compress.c"
      "zstd-1.5.7/lib/compress/zstd_lazy.c"
      "zstd-1.5.7/lib/compress/zstd_compress_internal.h"
      "zstd-1.5.7/lib/compress/zstd_opt.c"
      "zstd-1.5.7/lib/decompress/huf_decompress.c"
      "zstd-1.5.7/lib/decompress/zstd_decompress_block.c"
      "zstd-1.5.7/lib/decompress/zstd_decompress.c"
      "zstd-1.5.7/lib/dictBuilder/divsufsort.c"
      "zstd-1.5.7/lib/legacy/zstd_v01.c" "zstd-1.5.7/lib/legacy/zstd_v06.c"
      "zstd-1.5.7/lib/legacy/zstd_v02.c" "zstd-1.5.7/lib/legacy/zstd_v03.c"
      "zstd-1.5.7/lib/legacy/zstd_v07.c" "zstd-1.5.7/lib/zdict.h"
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE extraction_result)
  if(NOT extraction_result EQUAL 0)
    message(FATAL_ERROR "Dependency-patch fixture extraction failed")
  endif()
  set(${output}
      "${root}/zstd-1.5.7"
      PARENT_SCOPE)
endfunction()

# Purpose: Require a child patch to reject one invalid fixture without
# overwrite. Inputs: root owns the fixture; cause is the exact expected failure
# diagnostic; optional third argument selects the relative source boundary.
# Outputs: Fails if the child succeeds, rejects for another reason, or changes
# the selected source.
function(require_rejection root cause)
  if(ARGC GREATER 2)
    set(relative "${ARGV2}")
  else()
    set(relative "lib/legacy/zstd_v05.c")
  endif()
  set(source "${root}/${relative}")
  file(SHA256 "${source}" before)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${REPO_ROOT}"
            "-DPATCH_SOURCE_DIR=${root}" -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${cause}")
    message(
      FATAL_ERROR "Invalid fixture was not rejected as expected: ${error}")
  endif()
  file(SHA256 "${source}" after)
  if(NOT before STREQUAL after)
    message(FATAL_ERROR "Rejected dependency patch changed source")
  endif()
endfunction()

prepare_fixture(previous previous)
set(_previous_source "${previous}/lib/legacy/zstd_v05.c")
file(READ "${_previous_source}" previous_content)
string(
  CONCAT previous_contract
         "/* Purpose: Construct a fully owned buffered decoder, never a "
         "partial context.\n"
         " * Inputs: None; allocator failure is a normal error path.\n"
         " * Outputs: Returns a complete owner or NULL after releasing "
         "partial allocations. */\n")
string(REPLACE "ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
               "${previous_contract}ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
               previous_content "${previous_content}")
string(CONCAT previous_create "    zbc->zc = ZSTDv05_createDCtx();\n"
              "    if (zbc->zc==NULL) {\n        free(zbc);\n"
              "        return NULL;\n    }\n")
string(REPLACE "    zbc->zc = ZSTDv05_createDCtx();\n" "${previous_create}"
               previous_content "${previous_content}")
file(WRITE "${_previous_source}.fixture" "${previous_content}")
configure_file("${_previous_source}.fixture" "${_previous_source}" @ONLY
               NEWLINE_STYLE LF)
file(SHA256 "${_previous_source}" previous_hash)
if(NOT previous_hash STREQUAL
   "ee643222919c3a354da6f71d69f55c681ae0647a274384f2143afbe63dd5d9be")
  message(FATAL_ERROR "Earlier constructor-repair fixture identity mismatch")
endif()
superzip_patch_zstd_legacy("${previous}")

prepare_fixture(fresh fresh)
superzip_patch_zstd_legacy("${fresh}")

prepare_fixture(previous-literals previous_literals)
superzip_patch_zstd_foundation("${previous_literals}")
superzip_patch_zstd_legacy_literals("${previous_literals}")
superzip_patch_zstd_legacy("${previous_literals}")

prepare_fixture(previous-bufferless previous_bufferless)
superzip_patch_zstd_foundation("${previous_bufferless}")
file(SHA256 "${previous_bufferless}/lib/decompress/zstd_decompress.c"
     previous_bufferless_hash)
if(NOT previous_bufferless_hash STREQUAL
   "8ef204750636b93311fe4f0e6e2db66990e399ae78fc837dcf5c6c4c0d636354")
  message(FATAL_ERROR "Previous bufferless fixture identity mismatch")
endif()
superzip_patch_zstd_legacy("${previous_bufferless}")

# Purpose: Reconstruct exact historical stream bytes to test migration from both
# preceding revisions. Inputs: root owns a freshly patched fixture and
# keep_probes selects the historical read-only probe revision. Outputs:
# Publishes only hash-verified historical bytes below root; no decoder copy.
function(prepare_previous_stream root keep_probes)
  include("${REPO_ROOT}/cmake/ZstdLegacyStream.cmake")
  foreach(version IN ITEMS v05 v06 v07)
    set(stream_key "_zstd_legacy_stream_${version}")
    set(stream_source "${root}/lib/legacy/zstd_${version}.c")
    file(READ "${stream_source}" stream_content)
    string(FIND "${stream_content}"
                "\n\n/* Purpose: Measure a public legacy byte cursor"
                distance_begin)
    string(FIND "${stream_content}" "\n\n/* Purpose: Acquire stream buffers"
                distance_end)
    if(distance_begin LESS 0 OR distance_end LESS_EQUAL distance_begin)
      message(FATAL_ERROR "Previous empty stream fixture boundary mismatch")
    endif()
    string(SUBSTRING "${stream_content}" 0 ${distance_begin} prefix)
    string(SUBSTRING "${stream_content}" ${distance_end} -1 suffix)
    string(CONCAT stream_content "${prefix}" "${suffix}")
    foreach(pointer IN ITEMS "iend, *ip" "oend, *op" "iend, ip" "ip, istart"
                             "op, ostart")
      string(REPLACE ", " " - " difference "${pointer}")
      string(REPLACE "*ip" "(*ip)" difference "${difference}")
      string(REPLACE "*op" "(*op)" difference "${difference}")
      string(REPLACE "ZBUFF${version}_cursorDistance(${pointer})"
                     "${difference}" stream_content "${stream_content}")
    endforeach()
    string(REPLACE "*srcSizePtr != 0 ? istart + *srcSizePtr : istart"
                   "istart + *srcSizePtr" stream_content "${stream_content}")
    foreach(capacity IN ITEMS maxDstSizePtr dstCapacityPtr)
      string(REPLACE "*${capacity} != 0 ? ostart + *${capacity} : ostart"
                     "ostart + *${capacity}" stream_content "${stream_content}")
    endforeach()
    foreach(pair IN ITEMS "loadedSize" "flushedSize")
      string(REPLACE "if (${pair} != 0)\n        " "" stream_content
                     "${stream_content}")
    endforeach()
    string(REPLACE "if (headerSize != 0)\n                    " ""
                   stream_content "${stream_content}")
    string(REPLACE "if (*srcSizePtr != 0)\n                        " ""
                   stream_content "${stream_content}")
    if(keep_probes)
      set(expected "${${stream_key}_empty_prior}")
    else()
      string(FIND "${stream_content}" "\n\n#ifdef SUPERZIP_ZSTD_BUFFER_PROBES"
                  hook_begin)
      string(FIND "${stream_content}" "${${stream_key}_end}" hook_end)
      if(hook_begin LESS 0 OR hook_end LESS_EQUAL hook_begin)
        message(FATAL_ERROR "Previous stream fixture boundary mismatch")
      endif()
      string(SUBSTRING "${stream_content}" 0 ${hook_begin} stream_prefix)
      string(SUBSTRING "${stream_content}" ${hook_end} -1 stream_suffix)
      string(CONCAT stream_content "${stream_prefix}" "${stream_suffix}")
      set(expected "${${stream_key}_prior}")
    endif()
    string(SHA256 stream_previous_hash "${stream_content}")
    if(NOT stream_previous_hash STREQUAL expected)
      message(FATAL_ERROR "Previous stream fixture identity mismatch")
    endif()
    file(WRITE "${stream_source}.fixture" "${stream_content}")
    configure_file("${stream_source}.fixture" "${stream_source}" @ONLY
                   NEWLINE_STYLE LF)
  endforeach()
endfunction()

prepare_fixture(previous-stream previous_stream)
superzip_patch_zstd_foundation("${previous_stream}")
superzip_patch_zstd_legacy_literals("${previous_stream}")
superzip_patch_zstd_legacy_stream("${previous_stream}")
prepare_previous_stream("${previous_stream}" FALSE)
superzip_patch_zstd_legacy("${previous_stream}")

prepare_fixture(previous-empty-stream previous_empty_stream)
superzip_patch_zstd_foundation("${previous_empty_stream}")
superzip_patch_zstd_legacy_literals("${previous_empty_stream}")
superzip_patch_zstd_legacy_stream("${previous_empty_stream}")
prepare_previous_stream("${previous_empty_stream}" TRUE)
superzip_patch_zstd_legacy("${previous_empty_stream}")

# Migrate the exact last stream revision before explicit history extents.
prepare_fixture(previous-history previous_history)
superzip_patch_zstd_foundation("${previous_history}")
superzip_patch_zstd_legacy_literals("${previous_history}")
superzip_patch_zstd_legacy_stream("${previous_history}")
include("${REPO_ROOT}/cmake/ZstdLegacyHistory.cmake")
foreach(version IN ITEMS v05 v06 v07)
  set(_history_key "_zstd_legacy_history_${version}")
  file(SHA256 "${previous_history}/lib/legacy/zstd_${version}.c" history_hash)
  if(NOT history_hash STREQUAL "${${_history_key}_original}")
    message(FATAL_ERROR "Previous legacy history fixture identity mismatch")
  endif()
endforeach()
superzip_patch_zstd_legacy("${previous_history}")

# Migrate the exact history rewrite before dictionary error propagation.
prepare_fixture(previous-dictionary previous_dictionary)
superzip_patch_zstd_foundation("${previous_dictionary}")
superzip_patch_zstd_legacy_literals("${previous_dictionary}")
superzip_patch_zstd_legacy_stream("${previous_dictionary}")
superzip_patch_zstd_legacy_history("${previous_dictionary}")
include("${REPO_ROOT}/cmake/ZstdLegacyDictionary.cmake")
foreach(version IN ITEMS v05 v06 v07)
  set(_dictionary_key "_zstd_legacy_dictionary_${version}")
  file(SHA256 "${previous_dictionary}/lib/legacy/zstd_${version}.c"
       dictionary_hash)
  if(NOT dictionary_hash STREQUAL "${${_dictionary_key}_original}")
    message(FATAL_ERROR "Previous legacy dictionary fixture identity mismatch")
  endif()
endforeach()
superzip_patch_zstd_legacy("${previous_dictionary}")

prepare_fixture(previous-allocator previous_allocator)
superzip_patch_zstd_legacy("${previous_allocator}")
set(_previous_allocator_source "${previous_allocator}/lib/common/allocations.h")
file(READ "${_previous_allocator_source}" allocator_content)
set(_allocator_guard
    "#ifndef ZSTD_ALLOCATIONS_H\n#define ZSTD_ALLOCATIONS_H\n\n")
string(REPLACE "${_allocator_guard}" "" allocator_content
               "${allocator_content}")
string(REPLACE "/* custom memory allocation functions */"
               "${_allocator_guard}/* custom memory allocation functions */"
               allocator_content "${allocator_content}")
string(SHA256 allocator_previous_hash "${allocator_content}")
if(NOT allocator_previous_hash STREQUAL
   "b1e3a6f3460a0558727860cd0ade078c2311cc474c6a4aafc161bdf938c296cd")
  message(FATAL_ERROR "Previous allocator fixture identity mismatch")
endif()
file(WRITE "${_previous_allocator_source}.fixture" "${allocator_content}")
configure_file("${_previous_allocator_source}.fixture"
               "${_previous_allocator_source}" @ONLY NEWLINE_STYLE LF)
superzip_patch_zstd_legacy("${previous_allocator}")

# Recreate the exact preceding COVER cleanup revision independently of the new
# migration branch, then require migration to match a fresh provenance repair.
prepare_fixture(previous-cover previous_cover)
set(_previous_cover_source "${previous_cover}/lib/dictBuilder/cover.c")
file(READ "${_previous_cover_source}" original_cover)
string(FIND "${original_cover}" "COVER_dictSelection_t COVER_selectDict("
            original_begin)
string(FIND "${original_cover}" "/**\n * Parameters for COVER_tryParameters()."
            original_end)
math(EXPR original_length "${original_end} - ${original_begin}")
string(SUBSTRING "${original_cover}" ${original_begin} ${original_length}
                 original_selection)
file(READ "${REPO_ROOT}/cmake/ZstdCoverSelection.c" repaired_selection)
file(READ "${fresh}/lib/dictBuilder/cover.c" repaired_cover)
string(REPLACE "${repaired_selection}\n" "${original_selection}"
               previous_cover_content "${repaired_cover}")
file(WRITE "${_previous_cover_source}.fixture" "${previous_cover_content}")
configure_file("${_previous_cover_source}.fixture" "${_previous_cover_source}"
               @ONLY NEWLINE_STYLE LF)
file(SHA256 "${_previous_cover_source}" previous_cover_hash)
if(NOT previous_cover_hash STREQUAL
   "89a86e4217d306fd236bfb41cbdb7081e13082d88acc0f780a713df46da3b0da")
  message(FATAL_ERROR "Earlier COVER cleanup fixture identity mismatch")
endif()
superzip_patch_zstd_legacy("${previous_cover}")
file(SHA256 "${_previous_cover_source}" migrated_cover_hash)
file(SHA256 "${fresh}/lib/dictBuilder/cover.c" fresh_cover_hash)
if(NOT migrated_cover_hash STREQUAL fresh_cover_hash)
  message(FATAL_ERROR "COVER cleanup migration differs from fresh repair")
endif()
set(BOUNDARIES
    "lib/legacy/zstd_v05.c"
    "lib/legacy/zstd_legacy.h"
    "lib/common/allocations.h"
    "lib/dictBuilder/cover.h"
    "lib/compress/hist.h"
    "lib/legacy/zstd_v04.c"
    "lib/compress/zstdmt_compress.c"
    "lib/dictBuilder/cover.c"
    "lib/dictBuilder/zdict.c"
    "lib/common/entropy_common.c"
    "lib/common/huf.h"
    "lib/common/xxhash.h"
    "lib/compress/fse_compress.c"
    "lib/compress/zstd_compress.c"
    "lib/compress/huf_compress.c"
    "lib/compress/zstd_lazy.c"
    "lib/compress/zstd_compress_internal.h"
    "lib/compress/zstd_opt.c"
    "lib/decompress/huf_decompress.c"
    "lib/decompress/zstd_decompress_block.c"
    "lib/decompress/zstd_decompress.c"
    "lib/dictBuilder/divsufsort.c"
    "lib/legacy/zstd_v01.c"
    "lib/legacy/zstd_v02.c"
    "lib/legacy/zstd_v03.c"
    "lib/legacy/zstd_v06.c"
    "lib/legacy/zstd_v07.c"
    "lib/zdict.h")
prepare_fixture(prior_comments prior_comments)
superzip_patch_zstd_foundation("${prior_comments}")
include("${REPO_ROOT}/cmake/ZstdCommentClarifications.cmake")
math(EXPR last_comment_file "${_zstd_comment_file_count} - 1")
foreach(comment_file RANGE 0 ${last_comment_file} 1)
  set(_comment_key "_zstd_comment_${comment_file}")
  set(_comment_source "${prior_comments}/${${_comment_key}_path}")
  file(READ "${_comment_source}" content)
  math(EXPR last_comment "${${_comment_key}_count} - 1")
  foreach(comment_index RANGE 0 ${last_comment} 1)
    if(comment_index GREATER_EQUAL "${${_comment_key}_prior_count}")
      string(REPLACE "${${_comment_key}_${comment_index}_new}"
                     "${${_comment_key}_${comment_index}_old}" content
                     "${content}")
    endif()
  endforeach()
  string(SHA256 prior_hash "${content}")
  if(NOT prior_hash STREQUAL "${${_comment_key}_prior}")
    message(FATAL_ERROR "Previous comment fixture identity mismatch")
  endif()
  file(WRITE "${_comment_source}.fixture" "${content}")
  configure_file("${_comment_source}.fixture" "${_comment_source}" @ONLY
                 NEWLINE_STYLE LF)
  file(SHA256 "${_comment_source}" written_prior_hash)
  if(NOT written_prior_hash STREQUAL "${${_comment_key}_prior}")
    message(FATAL_ERROR "Previous comment fixture write identity mismatch")
  endif()
endforeach()
superzip_patch_zstd_legacy("${prior_comments}")
foreach(boundary IN LISTS BOUNDARIES)
  string(MAKE_C_IDENTIFIER "${boundary}" key)
  file(SHA256 "${fresh}/${boundary}" "before_${key}")
  file(SHA256 "${previous}/${boundary}" migrated)
  if(NOT "${before_${key}}" STREQUAL migrated)
    message(
      FATAL_ERROR "Incremental patch differs from fresh source: ${boundary}")
  endif()
  file(SHA256 "${previous_literals}/${boundary}" migrated_literals)
  if(NOT "${before_${key}}" STREQUAL migrated_literals)
    message(
      FATAL_ERROR "Previous literal-decoder migration differs from fresh source"
    )
  endif()
  file(SHA256 "${previous_bufferless}/${boundary}" migrated_bufferless)
  if(NOT "${before_${key}}" STREQUAL migrated_bufferless)
    message(
      FATAL_ERROR "Previous bufferless migration differs from fresh source")
  endif()
  file(SHA256 "${previous_allocator}/${boundary}" migrated_allocator)
  if(NOT "${before_${key}}" STREQUAL migrated_allocator)
    message(
      FATAL_ERROR "Previous allocator migration differs from fresh source")
  endif()
  file(SHA256 "${previous_stream}/${boundary}" migrated_stream)
  if(NOT "${before_${key}}" STREQUAL migrated_stream)
    message(FATAL_ERROR "Previous stream migration differs from fresh source")
  endif()
  file(SHA256 "${previous_empty_stream}/${boundary}" migrated_empty_stream)
  if(NOT "${before_${key}}" STREQUAL migrated_empty_stream)
    message(
      FATAL_ERROR "Previous empty stream migration differs from fresh source")
  endif()
  file(SHA256 "${previous_history}/${boundary}" migrated_history)
  if(NOT "${before_${key}}" STREQUAL migrated_history)
    message(
      FATAL_ERROR "Previous legacy history migration differs from fresh source")
  endif()
  file(SHA256 "${previous_dictionary}/${boundary}" migrated_dictionary)
  if(NOT "${before_${key}}" STREQUAL migrated_dictionary)
    message(
      FATAL_ERROR "Previous dictionary migration differs from fresh source")
  endif()
  file(SHA256 "${prior_comments}/${boundary}" migrated_comments)
  if(NOT "${before_${key}}" STREQUAL migrated_comments)
    message(FATAL_ERROR "Previous comment migration differs from fresh source")
  endif()
endforeach()
superzip_patch_zstd_legacy("${fresh}")
foreach(boundary IN LISTS BOUNDARIES)
  string(MAKE_C_IDENTIFIER "${boundary}" key)
  file(SHA256 "${fresh}/${boundary}" repeated)
  if(NOT "${before_${key}}" STREQUAL repeated)
    message(FATAL_ERROR "Dependency patch is not idempotent: ${boundary}")
  endif()
  prepare_fixture("drift-${key}" drift)
  file(APPEND "${drift}/${boundary}" "\n/* fixture source drift */\n")
  require_rejection("${drift}" "Zstandard patch source identity mismatch"
                    "${boundary}")
  foreach(phase IN ITEMS input output)
    string(MAKE_C_IDENTIFIER "${boundary}" boundary_name)
    prepare_fixture("interrupted-${boundary_name}-${phase}" interrupted)
    set(PARTIAL "${interrupted}/${boundary}.superzip-patch")
    if(phase STREQUAL "input")
      string(APPEND PARTIAL ".raw")
    endif()
    file(WRITE "${PARTIAL}" "fixture-owned incomplete patch\n")
    file(SHA256 "${PARTIAL}" partial_before)
    require_rejection("${interrupted}"
                      "Incomplete or concurrent Zstandard patch" "${boundary}")
    file(SHA256 "${PARTIAL}" partial_after)
    if(NOT partial_before STREQUAL partial_after)
      message(FATAL_ERROR "Dependency patch overwrote an incomplete patch")
    endif()
  endforeach()
endforeach()

file(SHA256 "${ARCHIVE}" archive_after)
if(NOT archive_before STREQUAL archive_after)
  message(FATAL_ERROR "Dependency patch changed the provenance archive")
endif()
string(CONCAT success_message "Zstandard patch fresh/idempotent/drift/"
              "interruption/provenance checks passed")
message(STATUS "${success_message}")
