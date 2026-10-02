# Purpose: Validate original and repaired headers using an isolated source tree.
# Inputs: REPO_ROOT/BINARY_DIR and parent generator settings select the native
# compiler; source archive identity is pinned before extraction. Outputs: Fails
# on a broken guard/API contract; preserves upstream provenance.
if(NOT DEFINED REPO_ROOT
   OR NOT DEFINED BINARY_DIR
   OR NOT DEFINED GENERATOR)
  message(FATAL_ERROR "REPO_ROOT, BINARY_DIR and GENERATOR are required")
endif()
set(ARCHIVE "${REPO_ROOT}/third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip")
file(SHA256 "${ARCHIVE}" before)
if(NOT before STREQUAL
   "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4")
  message(FATAL_ERROR "Header-contract source archive identity mismatch")
endif()
string(
  RANDOM
  LENGTH 16
  ALPHABET 0123456789abcdef nonce)
set(SCRATCH "${BINARY_DIR}/tests/zstd-header-contracts-${nonce}")
if(EXISTS "${SCRATCH}")
  message(FATAL_ERROR "Refusing a preexisting header-contract fixture")
endif()
file(MAKE_DIRECTORY "${SCRATCH}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xf "${ARCHIVE}" -- "zstd-1.5.7/lib"
  WORKING_DIRECTORY "${SCRATCH}"
  RESULT_VARIABLE extraction_result
  TIMEOUT 30)
if(NOT extraction_result STREQUAL "0")
  message(FATAL_ERROR "Header-contract fixture extraction failed")
endif()
set(GENERATOR_ARGS -G "${GENERATOR}")
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
  list(APPEND GENERATOR_ARGS -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
  list(APPEND GENERATOR_ARGS -T "${GENERATOR_TOOLSET}")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${REPO_ROOT}/tests/zstd/headers" -B
    "${SCRATCH}/build" ${GENERATOR_ARGS} "-DREPO_ROOT=${REPO_ROOT}"
    "-DSOURCE_ROOT=${SCRATCH}/zstd-1.5.7"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 120)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "Header-contract compilation failed: ${output}${error}")
endif()
file(SHA256 "${ARCHIVE}" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "Header-contract fixture changed upstream provenance")
endif()
message(STATUS "${output}")
