cmake_minimum_required(VERSION 4.4)
project(SuperZipZstdHeaderContracts LANGUAGES C CXX)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "REPO_ROOT and SOURCE_ROOT are required")
endif()
include("${REPO_ROOT}/cmake/PatchZstdLegacy.cmake")

# Purpose: Compile the same header contract in each supported language/mode.
# Inputs: name identifies isolated output; extension selects C/C++; repeat
# controls private-header re-inclusion; expected_success is the required result.
# Outputs: Fails on unexpected compilation or an unrelated negative diagnostic.
function(require_header_compilation name extension repeat expected_success)
  try_compile(
    compiled SOURCE_FROM_FILE
    "contract.${extension}" "${CMAKE_CURRENT_SOURCE_DIR}/header_contract.c"
    NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib"
    COMPILE_DEFINITIONS
      -DZSTD_MULTITHREAD "-DSUPERZIP_REPEAT_PRIVATE_HEADERS=${repeat}"
      CXX_STANDARD 20 C_STANDARD 11
    OUTPUT_VARIABLE diagnostic LOG_DESCRIPTION "Zstandard ${name}")
  if(expected_success AND NOT compiled)
    message(FATAL_ERROR "Header contract ${name} failed: ${diagnostic}")
  elseif(NOT expected_success)
    if(compiled
       OR NOT diagnostic MATCHES "COVER_"
       OR NOT diagnostic MATCHES "redefinition|conflicting|C2011")
      message(FATAL_ERROR "Negative control ${name} failed: ${diagnostic}")
    endif()
  endif()
endfunction()

foreach(extension IN ITEMS c cpp)
  require_header_compilation("original-staged-${extension}" "${extension}" 0
                             TRUE)
  require_header_compilation("original-repeated-${extension}" "${extension}" 1
                             FALSE)
endforeach()
superzip_patch_zstd_legacy("${SOURCE_ROOT}")
foreach(extension IN ITEMS c cpp)
  require_header_compilation("patched-repeated-${extension}" "${extension}" 1
                             TRUE)
endforeach()
message(STATUS "Zstandard C/C++ staged and repeated header contracts passed")
