cmake_minimum_required(VERSION 4.4)
project(SuperZipZstdHeaderContracts LANGUAGES C CXX)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "REPO_ROOT and SOURCE_ROOT are required")
endif()
include("${REPO_ROOT}/cmake/PatchZstdLegacy.cmake")
include("${REPO_ROOT}/cmake/ZstdHeaderComponents.cmake")

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

# Purpose: Link actual common/error source with independent C and C++ clients.
# Inputs: The isolated original or repaired dependency; expected_success selects
# the storage-sharing result. Outputs: Requires unchanged null callbacks and one
# object address; original per-unit copies form a failing runtime control.
function(require_default_allocator_sharing expected_success)
  try_run(
    ran
    compiled
    SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/default_allocator_contract.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/default_allocator_a.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/default_allocator_b.cpp"
    "${SOURCE_ROOT}/lib/common/zstd_common.c"
    "${SOURCE_ROOT}/lib/common/error_private.c"
    NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib" C_STANDARD 11
                CXX_STANDARD 20
    COMPILE_OUTPUT_VARIABLE diagnostic)
  if(NOT compiled)
    message(FATAL_ERROR "Default allocator linkage failed: ${diagnostic}")
  endif()
  if((expected_success AND NOT ran EQUAL 0)
     OR (NOT expected_success AND NOT ran EQUAL 1))
    message(FATAL_ERROR "Default allocator storage contract failed: ${ran}")
  endif()
endfunction()

require_default_allocator_sharing(FALSE)

# Reconstruct the exact published predecessor only inside this isolated fixture.
# Both phases then preprocess the same include paths, without changing the
# public dispatcher or using a production opt-out for specialization.
set(HASH_SOURCE "${SOURCE_ROOT}/lib/common/xxhash.h")
file(READ "${HASH_SOURCE}" HASH_CONTENT)
include("${REPO_ROOT}/cmake/ZstdCommentClarifications.cmake")
if(NOT _zstd_comment_2_path STREQUAL "lib/common/xxhash.h")
  message(FATAL_ERROR "Hash predecessor annotation record changed")
endif()
math(EXPR LAST_HASH_COMMENT "${_zstd_comment_2_count} - 1")
foreach(index RANGE 0 ${LAST_HASH_COMMENT} 1)
  string(REPLACE "${_zstd_comment_2_${index}_old}"
                 "${_zstd_comment_2_${index}_new}" HASH_CONTENT
                 "${HASH_CONTENT}")
endforeach()
superzip_write_verified_zstd_patch(
  "${HASH_SOURCE}" "${_zstd_comment_2_original}" "${_zstd_comment_2_patched}"
  "${HASH_CONTENT}")
superzip_split_zstd_hash_header("${HASH_CONTENT}"
                                "_zstd_header_component_xxhash")
foreach(part IN ITEMS public static implementation)
  set(EXPECTED "${_zstd_header_component_xxhash_${part}_hash}")
  if(part STREQUAL "implementation")
    set(EXPECTED "${_zstd_header_component_xxhash_implementation_original}")
  endif()
  superzip_publish_zstd_header_component(
    "${SOURCE_ROOT}/lib/common/xxhash_${part}.h" "${EXPECTED}" "${${part}}")
endforeach()
superzip_write_verified_zstd_patch(
  "${HASH_SOURCE}" "${_zstd_header_component_xxhash_original}"
  "${_zstd_header_component_xxhash_patched}" "${dispatch}")

# Purpose: Compare complete compiler-visible code in every supported API mode.
# Inputs: phase selects predecessor or owned implementation; extension/mode
# select C/C++ and external, private, inline, staged or 32-bit-only hashing.
# Outputs: Requires identical preprocessed tokens, apart from the independently
# tested C++32 linkage repair; errors and oversized outputs reject.
function(require_hash_preprocessing phase extension mode)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/hash-${phase}-${extension}-${mode}.i")
  set(source "${CMAKE_CURRENT_SOURCE_DIR}/hash_contract.c")
  if(extension STREQUAL "c")
    set(compiler "${CMAKE_C_COMPILER}")
    set(language c)
    set(standard_option "${CMAKE_C11_STANDARD_COMPILE_OPTION}")
    set(language_flag /TC)
  else()
    set(compiler "${CMAKE_CXX_COMPILER}")
    set(language c++)
    set(standard_option "${CMAKE_CXX20_STANDARD_COMPILE_OPTION}")
    set(language_flag /TP)
  endif()
  if(MSVC)
    set(options
        /nologo
        /EP
        "/I${SOURCE_ROOT}/lib"
        "/DSUPERZIP_HASH_MODE=${mode}"
        /DNDEBUG
        ${language_flag}
        ${standard_option})
    foreach(directory IN LISTS hash_system_includes_${extension})
      list(APPEND options "/I${directory}")
    endforeach()
  elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(options
        -E
        -P
        "-I${SOURCE_ROOT}/lib"
        "-DSUPERZIP_HASH_MODE=${mode}"
        -DNDEBUG
        -x
        "${language}"
        ${standard_option})
  else()
    message(FATAL_ERROR "Unsupported hash-contract preprocessor")
  endif()
  execute_process(
    COMMAND "${compiler}" ${options} "${source}"
    RESULT_VARIABLE result
    OUTPUT_FILE "${output}"
    ERROR_VARIABLE diagnostic
    TIMEOUT 30)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Hash preprocessing failed: ${diagnostic}")
  endif()
  file(SIZE "${output}" size)
  if(size LESS 1 OR size GREATER 1048576)
    message(
      FATAL_ERROR "Hash preprocessor output is empty or exceeds its bound")
  endif()
  file(READ "${output}" content)
  # Preserve quoted literals exactly while removing insignificant spacing.
  string(
    REGEX MATCHALL
          "\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'|[A-Za-z_0-9]+|[^ \t\r\n]"
          tokens "${content}")
  if(phase STREQUAL "patched"
     AND extension STREQUAL "cpp"
     AND mode EQUAL 4)
    # This is the sole intended token change: close the original unclosed C
    # linkage before the first fixture function. Runtime controls require the
    # original header to fail and the repaired header to compile and execute.
    string(REPLACE "};static;int;check32" "static;int;check32" tokens
                   "${tokens}")
  endif()
  string(SHA256 actual "${tokens}")
  if(phase STREQUAL "original")
    set(hash_preprocessing_${extension}_${mode}
        "${actual}"
        PARENT_SCOPE)
  elseif(NOT actual STREQUAL "${hash_preprocessing_${extension}_${mode}}")
    message(FATAL_ERROR "Hash specialization changes compiler-visible code: "
                        "${extension} mode ${mode}")
  endif()
endfunction()

# Purpose: Execute classic hashing against bounded stream/canonical oracles.
# Inputs: Original or owned dependency, C/C++ and external or 32-bit-only mode.
# Outputs: Requires successful linkage and 98,328 agreeing runtime cases; test
# instances allocate only a small fixed stack buffer.
function(require_hash_runtime phase extension mode)
  set(definitions "-DSUPERZIP_HASH_MODE=${mode}")
  if(MSVC)
    list(APPEND definitions /showIncludes)
  endif()
  try_run(
    ran compiled SOURCE_FROM_FILE "hash.${extension}"
    "${CMAKE_CURRENT_SOURCE_DIR}/hash_contract.c" NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib"
    COMPILE_DEFINITIONS ${definitions} C_STANDARD 11 CXX_STANDARD 20
    COMPILE_OUTPUT_VARIABLE diagnostic)
  if(phase STREQUAL "original"
     AND extension STREQUAL "cpp"
     AND mode EQUAL 4)
    if(compiled OR NOT diagnostic MATCHES
                   "C1075|expected.*end of input|expected.*[}]")
      message(
        FATAL_ERROR "Original C++32 linkage control failed: ${diagnostic}")
    endif()
    return()
  endif()
  if(NOT compiled OR NOT ran STREQUAL "0")
    message(FATAL_ERROR "Classic hash runtime contract failed: "
                        "${extension} mode ${mode}: ${ran}; ${diagnostic}")
  endif()
  if(MSVC)
    # Discover the selected compiler's include closure through MSBuild; direct
    # cl invocation must not guess host SDK or Visual Studio paths.
    string(REGEX MATCHALL "[A-Za-z]:[/\\\\][^\r\n]*\\.h" includes
                 "${diagnostic}")
    if(NOT includes)
      message(FATAL_ERROR "MSVC hash include closure is unavailable")
    endif()
    set(directories "${hash_system_includes_${extension}}")
    foreach(header IN LISTS includes)
      if(NOT EXISTS "${header}")
        message(FATAL_ERROR "MSVC reported an unavailable include: ${header}")
      endif()
      get_filename_component(directory "${header}" DIRECTORY)
      list(APPEND directories "${directory}")
    endforeach()
    list(REMOVE_DUPLICATES directories)
    set(hash_system_includes_${extension}
        "${directories}"
        PARENT_SCOPE)
  endif()
endfunction()

# Purpose: Require the same preprocessed ABI and exact runtime semantics.
# Inputs: phase selects the original comparison or the repaired verification.
# Outputs: Covers five modes in both languages and all compiled classic APIs.
function(require_classic_hash_contracts phase)
  foreach(extension IN ITEMS c cpp)
    set(hash_system_includes_${extension})
    foreach(mode IN ITEMS 0 4)
      require_hash_runtime("${phase}" "${extension}" "${mode}")
    endforeach()
    foreach(mode IN ITEMS 0 1 2 3 4)
      require_hash_preprocessing("${phase}" "${extension}" "${mode}")
      if(phase STREQUAL "original")
        set(hash_preprocessing_${extension}_${mode}
            "${hash_preprocessing_${extension}_${mode}}"
            PARENT_SCOPE)
      endif()
    endforeach()
    string(CONCAT unsupported_source "#include \"common/xxhash.h\"\n"
                  "int main(void) { return (int)sizeof(XXH3_state_t); }\n")
    try_compile(
      unavailable SOURCE_FROM_CONTENT
      "unavailable.${extension}" "${unsupported_source}" NO_CACHE
      CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib" C_STANDARD
                  11 CXX_STANDARD 20
      OUTPUT_VARIABLE diagnostic)
    if(unavailable OR NOT diagnostic MATCHES "XXH3_state_t")
      message(FATAL_ERROR "XXH3 must remain unavailable in the Zstandard API: "
                          "compiled=${unavailable}; ${diagnostic}")
    endif()
  endforeach()
endfunction()

require_classic_hash_contracts(original)
superzip_patch_zstd_legacy("${SOURCE_ROOT}")
require_classic_hash_contracts(patched)
require_default_allocator_sharing(TRUE)
foreach(extension IN ITEMS c cpp)
  require_header_compilation("patched-repeated-${extension}" "${extension}" 1
                             TRUE)
  try_run(
    allocations_ran
    allocations_compiled
    SOURCE_FROM_FILE
    "allocations.${extension}"
    "${CMAKE_CURRENT_SOURCE_DIR}/allocations_contract.c"
    SOURCES
    "${SOURCE_ROOT}/lib/common/zstd_common.c"
    "${SOURCE_ROOT}/lib/common/error_private.c"
    NO_CACHE
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${SOURCE_ROOT}/lib" CXX_STANDARD
                20 C_STANDARD 11
    COMPILE_OUTPUT_VARIABLE allocations_diagnostic LOG_DESCRIPTION
                            "Zstandard private allocations first ${extension}")
  if(NOT allocations_compiled)
    message(
      FATAL_ERROR "Allocation header contract failed: ${allocations_diagnostic}"
    )
  endif()
  if(NOT allocations_ran EQUAL 0)
    message(FATAL_ERROR "Allocation ownership contract failed: ${extension}")
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
