# Purpose: Compile the canonical raw writer without importing unrelated codec
# state. Inputs: source is the verified generated compression header; output
# belongs to the test build. Outputs: Preserves the complete writer and exact
# upstream declarations, with separate provenance for each imported region.
function(superzip_generate_zstd_raw_block_probe source output)
  file(SHA256 "${source}" source_hash)
  if(NOT source_hash STREQUAL
     "94551c31098dd5029efd64d7fc3e7688a75e5d774c59cc9f2f1b82fc4ecb4662"
     AND NOT source_hash STREQUAL
         "c3bb0914e9f69bbb997492527308802c1f97d29294310998990277b163f6af46")
    message(FATAL_ERROR "Raw block probe source identity mismatch")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "MEM_STATIC size_t\nZSTD_noCompressBlock(" begin)
  string(FIND "${content}" "MEM_STATIC size_t\nZSTD_rleCompressBlock(" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Raw block probe source boundaries changed")
  endif()
  math(EXPR length "${end} - ${begin}")
  string(SUBSTRING "${content}" ${begin} ${length} writer)
  get_filename_component(compress_directory "${source}" DIRECTORY)
  string(CONCAT declaration_pattern "^#define ZSTD_BLOCKHEADERSIZE |"
                "^static UNUSED_ATTR const size_t ZSTD_blockHeaderSize |"
                "^typedef enum \\{ bt_raw,")
  file(STRINGS "${compress_directory}/../common/zstd_internal.h" declarations
       REGEX "${declaration_pattern}")
  list(LENGTH declarations declaration_count)
  if(NOT declaration_count EQUAL 3)
    message(FATAL_ERROR "Raw block probe declarations changed")
  endif()
  string(JOIN "\n" definitions ${declarations})
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/raw_block_probe.c" bridge)
  string(CONCAT probe "#include \"common/mem.h\"\n"
                "#include \"common/error_private.h\"\n${definitions}\n"
                "${writer}\n${bridge}")
  file(WRITE "${output}" "${probe}")
  string(SHA256 region_hash "${writer}")
  string(SHA256 declarations_hash "${definitions}")
  string(SHA256 bridge_hash "${bridge}")
  string(
    CONCAT identity
           "source_sha256=${source_hash}\n"
           "region_sha256=${region_hash}\n"
           "declarations_sha256=${declarations_hash}\n"
           "bridge_sha256=${bridge_hash}\n")
  file(WRITE "${output}.source-identity" "${identity}")
endfunction()
