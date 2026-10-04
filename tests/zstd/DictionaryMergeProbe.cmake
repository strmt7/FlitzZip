# Purpose: Compile exact private merger source with a bounded test bridge.
# Inputs: source is the generated product zdict.c; output is a test-owned C
# file. Outputs: Writes the original type and complete helper/merger region
# verbatim, retaining their source identities beside the probe. No alternate
# algorithm.
function(superzip_generate_zstd_dictionary_probe source output)
  file(SHA256 "${source}" source_hash)
  if(NOT source_hash STREQUAL
     "dbe57910c9d446bbf2195f31bdba5cffef2c80b93c0325e9df8c9ffa744b0aa3"
     AND NOT source_hash STREQUAL
         "cc4d13b0ec7b765addbcab3a373c176e96567018ce1394f8ef71ffd2fa7f8c20")
    message(FATAL_ERROR "Dictionary merge probe source identity mismatch")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "typedef struct {\n    U32 pos;" type_begin)
  string(FIND "${content}" "} dictItem;" type_end)
  string(FIND "${content}" "static int isIncluded(" merge_begin)
  string(FIND "${content}" "static void ZDICT_removeDictItem(" merge_end)
  if(type_begin LESS 0
     OR type_end LESS_EQUAL type_begin
     OR merge_begin LESS 0
     OR merge_end LESS_EQUAL merge_begin)
    message(FATAL_ERROR "Dictionary merge probe source boundaries changed")
  endif()
  math(EXPR type_length "${type_end} - ${type_begin} + 11")
  math(EXPR merge_length "${merge_end} - ${merge_begin}")
  string(SUBSTRING "${content}" ${type_begin} ${type_length} item_type)
  string(SUBSTRING "${content}" ${merge_begin} ${merge_length} merge_body)
  get_filename_component(builder_directory "${source}" DIRECTORY)
  file(STRINGS "${builder_directory}/../common/zstd_internal.h" bounds_macros
       REGEX "^#define (MIN|MAX)\\(a,b\\) ")
  list(LENGTH bounds_macros macro_count)
  if(NOT macro_count EQUAL 2)
    message(FATAL_ERROR "Dictionary merge probe arithmetic definitions changed")
  endif()
  string(JOIN "\n" bounds_definitions ${bounds_macros})
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/dictionary_merge_probe.c"
       bridge)
  string(CONCAT probe "#include \"common/mem.h\"\n${bounds_definitions}\n"
                "${item_type}\n${merge_body}\n${bridge}")
  file(WRITE "${output}" "${probe}")
  string(SHA256 region_hash "${item_type}\n${merge_body}")
  string(SHA256 arithmetic_hash "${bounds_definitions}")
  string(CONCAT identity "source_sha256=${source_hash}\n"
                "region_sha256=${region_hash}\n"
                "arithmetic_sha256=${arithmetic_hash}\n")
  file(WRITE "${output}.source-identity" "${identity}")
endfunction()
