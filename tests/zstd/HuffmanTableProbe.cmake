# Purpose: Compile the exact generated typed Huffman fill without a second
# implementation. Inputs: Verified complete decoder and test output path.
# Outputs: A private bridge with full-source and imported-region identity
# evidence.
function(superzip_generate_zstd_huffman_probe source output)
  include("${SUPERZIP_SOURCE_ROOT}/cmake/ZstdTableGeometry.cmake")
  file(SHA256 "${source}" actual)
  if(NOT actual STREQUAL _zstd_table_geometry_huffman_patched)
    message(FATAL_ERROR "Huffman table probe source identity mismatch")
  endif()
  file(READ "${source}" content)
  string(FIND "${content}" "typedef struct {\n    HUF_DEltX1* entries;" begin)
  string(FIND "${content}" "/**\n * Increase the tableLog" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Huffman table probe boundaries changed")
  endif()
  math(EXPR length "${end} - ${begin}")
  string(SUBSTRING "${content}" ${begin} ${length} helper)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/huffman_table_probe.c" bridge)
  string(
    CONCAT
      probe
      "#include \"common/mem.h\"\n"
      "#include \"common/error_private.h\"\n#define HUF_STATIC_LINKING_ONLY\n"
      "#include \"common/huf.h\"\n"
      "typedef struct { BYTE nbBits; BYTE byte; } HUF_DEltX1;\n"
      "${helper}\n${bridge}")
  file(WRITE "${output}" "${probe}")
  string(SHA256 region_hash "${helper}")
  string(SHA256 bridge_hash "${bridge}")
  string(CONCAT _zstd_emission_0 "source_sha256=${actual}\nregion_sha256="
                "${region_hash}\nbridge_sha256=${bridge_hash}" "\n")
  file(WRITE "${output}.source-identity" "${_zstd_emission_0}")
endfunction()
