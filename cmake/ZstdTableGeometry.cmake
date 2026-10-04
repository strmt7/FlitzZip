# Purpose: Replace packed wide stores with a fully checked typed Huffman table.
# Inputs: Complete pinned predecessor. Outputs: Proposes exact text and rejects
# missing boundaries; wire format and decoder entry representation stay intact.
function(superzip_rewrite_zstd_huffman_table content output)
  string(FIND "${content}" "/**\n * Packs 4 HUF_DEltX1 structs" begin)
  string(FIND "${content}" "/**\n * Increase the tableLog" end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard Huffman entry helper boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdHuffmanTable.c" helper)
  set(content "${prefix}${helper}\n${suffix}")
  string(FIND "${content}" "    /* fill DTable\n" begin)
  string(FIND "${content}" "    return iSize;\n}\n\nFORCE_INLINE_TEMPLATE BYTE"
              end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard Huffman table fill boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  string(
    CONCAT
      fill
      "    {\n"
      "        HUF_X1TableView const table = {dt, dtCapacity};\n"
      "        size_t const filled = HUF_fillX1Table(table, wksp->symbols, "
      "nbSymbols, wksp->rankVal, tableLog);\n"
      "        if (HUF_isError(filled)) return filled;\n    }\n")
  set(content "${prefix}${fill}${suffix}")
  string(REPLACE "    HUF_DEltX1* const dt = (HUF_DEltX1*)dtPtr;"
                 "${_zstd_huffman_view}" content "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish the exact typed Huffman table and legacy arithmetic
# revisions. Inputs: Extracted known generated sources. Outputs: Atomic verified
# revisions or an identity error; unknown sources and immutable provenance stay
# untouched.
function(superzip_patch_zstd_table_geometry source_dir)
  foreach(part IN ITEMS huffman legacy fse_header fse_builder fse_sequences)
    set(key "_zstd_table_geometry_${part}")
    set(source "${source_dir}/${${key}_path}")
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${${key}_patched}")
      continue()
    endif()
    if(NOT actual STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    if(part STREQUAL huffman)
      superzip_rewrite_zstd_huffman_table("${content}" content)
    elseif(part STREQUAL legacy)
      string(REPLACE "${_zstd_geometry_window_original}"
                     "${_zstd_geometry_window_checked}" content "${content}")
    else()
      include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdFseTableGeometry.cmake")
      superzip_rewrite_zstd_fse_table("${content}" "${part}" content)
    endif()
    superzip_write_verified_zstd_patch("${source}" "${actual}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

string(
  CONCAT _zstd_huffman_view
         "    HUF_DEltX1* const dt = (HUF_DEltX1*)dtPtr"
         ";\n"
         "    DTableDesc const initialDescriptor = HUF_"
         "getDTableDesc(DTable);\n"
         "    size_t dtCapacity;\n"
         "    if (initialDescriptor.maxTableLog > HUF_T"
         "ABLELOG_MAX) return ERROR(tableLog_tooLarge);"
         "\n"
         "    dtCapacity = (size_t)1 << (initialDescrip"
         "tor.maxTableLog + 1);")
string(
  CONCAT _zstd_geometry_window_original
         "    if (windowSize > (size_t)PTRDIFF_MAX - blockSize"
         " - WILDCOPY_OVERLENGTH * 2)\n"
         "        return ERROR(corruption_detected);")
set(_zstd_geometry_window_checked
    [=[#if PTRDIFF_MAX <= UINT32_MAX
    /* A 32-bit allocation extent needs a runtime check. On wider targets the
     * U32 window and bounded block fit by construction; the owner still checks
     * its actual requested capacities before allocation. */
    if (windowSize > (size_t)PTRDIFF_MAX - blockSize - WILDCOPY_OVERLENGTH * 2)
        return ERROR(corruption_detected);
#endif]=])
set(_zstd_table_geometry_huffman_path "lib/decompress/huf_decompress.c")
set(_zstd_table_geometry_huffman_original
    "cbac3cc9f9490f35c031587267193f419aefa6a6de352673e2ed008befc34d80")
set(_zstd_table_geometry_huffman_patched
    "e7041d30b1f039afd36e982d5796f963cd655246287af0f1924f16772a3ea071")
set(_zstd_table_geometry_legacy_path "lib/legacy/zstd_v07.c")
set(_zstd_table_geometry_legacy_original
    "c214c586b9f2dd031bb8f0857296c63f88000a26b77d454f3297c63a69f7da7b")
set(_zstd_table_geometry_legacy_patched
    "0385d8da21f32d780c02799786bef3491d00f7f06abdda340accd7aab2137adb")
set(_zstd_table_geometry_fse_header_path "lib/common/fse.h")
set(_zstd_table_geometry_fse_header_original
    "5235ce1e512bf80204d013b1f4cecd776fdaa9effddb48308bce08f6d0b84499")
set(_zstd_table_geometry_fse_header_patched
    "7e861d1f104b3bdb2e4a7a14e7e44418b7a05124c712d84d5d7255a8f4b32117")
set(_zstd_table_geometry_fse_builder_path "lib/compress/fse_compress.c")
set(_zstd_table_geometry_fse_builder_original
    "6dfd0803dcb03b6ce85fc180cef7ea306fe2eb91fa97e01f2511775fbbe733ea")
set(_zstd_table_geometry_fse_builder_patched
    "951491dab1b1f3169b679d501cbd7735ca9b1f01f1f65b159a5b3a1e4dad27ea")
set(_zstd_table_geometry_fse_sequences_path
    "lib/compress/zstd_compress_sequences.c")
set(_zstd_table_geometry_fse_sequences_original
    "3f4334c19ed770c4007bc12df4098026d4eb1a6fbebfbf36df58054e7babc3e9")
set(_zstd_table_geometry_fse_sequences_patched
    "e0f5346598c4d5d0a8c73751a9579bd344268acc4c3d8acdd49987eb37c8f5f2")
