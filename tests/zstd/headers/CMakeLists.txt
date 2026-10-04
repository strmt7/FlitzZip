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
  try_compile(
    allocations_compiled SOURCE_FROM_FILE
    "allocations.${extension}"
    "${CMAKE_CURRENT_SOURCE_DIR}/allocations_contract.c" NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib" CXX_STANDARD
                20 C_STANDARD 11
    OUTPUT_VARIABLE allocations_diagnostic LOG_DESCRIPTION
                    "Zstandard private allocations first ${extension}")
  if(NOT allocations_compiled)
    message(
      FATAL_ERROR "Allocation header contract failed: ${allocations_diagnostic}"
    )
  endif()
endforeach()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Purpose: Compile each complete encoder source with assertions enabled and
# disabled. Inputs: component selects the production translation unit; release
# controls NDEBUG. Outputs: Rejects missing assertion helpers or broken compile
# contracts in either mode.
function(require_assertion_helper_compilation component release)
  if(component STREQUAL "huf_compress")
    set(reference
        "static int (*const check_helper)(nodeElt*, U32) = &HUF_isSorted;")
  else()
    string(CONCAT reference
                  "static int (*const check_helper)(void const*, size_t) "
                  "= &ZSTD_isAligned;")
  endif()
  string(CONCAT source "#include \"compress/${component}.c\"\n"
                "#ifndef NDEBUG\n" "${reference}\n" "#endif\n")
  set(definitions -DDEBUGLEVEL=1 -DZSTD_DISABLE_ASM -D_CRT_SECURE_NO_WARNINGS)
  if(release)
    list(APPEND definitions -DNDEBUG)
  endif()
  try_compile(
    compiled SOURCE_FROM_CONTENT
    "assertions.c" "${source}" NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib"
    COMPILE_DEFINITIONS ${definitions} C_STANDARD 11
    OUTPUT_VARIABLE diagnostic LOG_DESCRIPTION
                    "Zstandard ${component} assertions release=${release}")
  if(NOT compiled)
    message(FATAL_ERROR "Assertion helper contract failed: ${diagnostic}")
  endif()
endfunction()
foreach(component IN ITEMS huf_compress zstd_lazy)
  foreach(release IN ITEMS TRUE FALSE)
    require_assertion_helper_compilation("${component}" "${release}")
  endforeach()
endforeach()
message(STATUS "Zstandard C/C++ staged and repeated header contracts passed")
