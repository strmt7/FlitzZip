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
      "zstd-1.5.7/lib/dictBuilder/cover.c"
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
set(BOUNDARIES
    "lib/legacy/zstd_v05.c"
    "lib/legacy/zstd_legacy.h"
    "lib/common/allocations.h"
    "lib/dictBuilder/cover.h"
    "lib/compress/hist.h"
    "lib/legacy/zstd_v04.c"
    "lib/compress/zstdmt_compress.c"
    "lib/dictBuilder/cover.c")
foreach(boundary IN LISTS BOUNDARIES)
  string(MAKE_C_IDENTIFIER "${boundary}" key)
  file(SHA256 "${fresh}/${boundary}" "before_${key}")
  file(SHA256 "${previous}/${boundary}" migrated)
  if(NOT "${before_${key}}" STREQUAL migrated)
    message(
      FATAL_ERROR "Incremental patch differs from fresh source: ${boundary}")
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
