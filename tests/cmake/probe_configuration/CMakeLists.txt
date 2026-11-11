cmake_minimum_required(VERSION 3.24)
project(FlitzZipProbeConfigurationContract LANGUAGES NONE)

if(NOT DEFINED REPO_ROOT
   OR NOT DEFINED SOURCE_ROOT
   OR NOT DEFINED MODULE_ROOT)
  message(FATAL_ERROR "Probe graph requires explicit source and module roots")
endif()
set(FLITZZIP_SOURCE_ROOT "${REPO_ROOT}")
include("${MODULE_ROOT}/RawBlockProbe.cmake")
include("${MODULE_ROOT}/HuffmanTableProbe.cmake")
include("${MODULE_ROOT}/DictionaryMergeProbe.cmake")
flitzzip_generate_zstd_raw_block_probe(
  "${SOURCE_ROOT}/compress/zstd_compress_internal.h"
  "${CMAKE_CURRENT_BINARY_DIR}/raw.c")
flitzzip_generate_zstd_huffman_probe(
  "${SOURCE_ROOT}/decompress/huf_decompress.c"
  "${CMAKE_CURRENT_BINARY_DIR}/huffman.c")
flitzzip_generate_zstd_dictionary_probe(
  "${SOURCE_ROOT}/dictBuilder/zdict.c"
  "${CMAKE_CURRENT_BINARY_DIR}/dictionary.c")

get_property(
  configure_inputs
  DIRECTORY
  PROPERTY CMAKE_CONFIGURE_DEPENDS)
set(_required_inputs
    "${SOURCE_ROOT}/compress/zstd_compress_internal.h"
    "${SOURCE_ROOT}/decompress/huf_decompress.c"
    "${SOURCE_ROOT}/dictBuilder/zdict.c"
    "${SOURCE_ROOT}/common/zstd_internal.h"
    "${MODULE_ROOT}/raw_block_probe.c"
    "${MODULE_ROOT}/huffman_table_probe.c"
    "${MODULE_ROOT}/dictionary_merge_probe.c")
foreach(input IN LISTS _required_inputs)
  get_filename_component(absolute_input "${input}" ABSOLUTE)
  if(NOT absolute_input IN_LIST configure_inputs)
    message(FATAL_ERROR "Missing probe configure dependency: ${input}")
  endif()
endforeach()
