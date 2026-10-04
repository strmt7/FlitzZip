# Complete legacy dictionary boundaries; enclosing sources retain upstream
# notices and the exact historical wire formats.

# Purpose: Repair one-shot error propagation and short raw-dictionary loading.
# Inputs: content owns a verified history revision; version is v05-v07. Outputs:
# Returns the complete proposed source through output without writing files or
# changing the dictionary's borrowed lifetime.
function(superzip_zstd_rewrite_legacy_dictionary content version output)
  if(version STREQUAL v05)
    set(decode ZSTDv05_decompress_continueDCtx)
  else()
    set(decode "ZSTD${version}_decompressFrame")
  endif()
  set(one_shot
      [=[/* Purpose: Decode a legacy frame after dictionary setup succeeds.
 * Inputs: dctx owns state; dstCapacity/srcSize/dictSize bound borrowed
 * dst/src/dict buffers, which remain live through the call.
 * Outputs: Returns setup errors before output, or the frame decoder result. */
size_t ZSTD@version@_decompress_usingDict(ZSTD@version@_DCtx* dctx,
                                       void* dst, size_t dstCapacity,
                                       const void* src, size_t srcSize,
                                       const void* dict, size_t dictSize)
{
    size_t const initialized =
        ZSTD@version@_decompressBegin_usingDict(dctx, dict, dictSize);
    if (ZSTD@version@_isError(initialized)) return initialized;
    ZSTD@version@_checkContinuity(dctx, dst);
    return @decode@(dctx, dst, dstCapacity, src, srcSize);
}


]=])
  string(CONFIGURE "${one_shot}" one_shot @ONLY)
  superzip_zstd_replace_legacy_region(
    "${content}" "size_t ZSTD${version}_decompress_usingDict("
    "size_t ZSTD${version}_decompressDCtx(" "${one_shot}" content)
  if(NOT version STREQUAL v07)
    set(insertion
        [=[/* Purpose: Load bounded entropy or borrow raw content.
 * Inputs: dctx is reset; dictSize bytes of dict stay borrowed through decoding.
 * Outputs: References content or returns dictionary_corrupted within bounds. */
static size_t ZSTD@version@_decompress_insertDictionary(
    ZSTD@version@_DCtx* dctx, const void* dict, size_t dictSize)
{
    size_t entropySize;
    if (dictSize < sizeof(U32) ||
        MEM_readLE32(dict) != ZSTD@version@_DICT_MAGIC) {
        ZSTD@version@_refDictContent(dctx, dict, dictSize);
        return 0;
    }
    dict = (const BYTE*)dict + sizeof(U32);
    dictSize -= sizeof(U32);
    entropySize = ZSTD@version@_loadEntropy(dctx, dict, dictSize);
    if (ZSTD@version@_isError(entropySize) || entropySize > dictSize)
        return ERROR(dictionary_corrupted);
    dict = (const BYTE*)dict + entropySize;
    dictSize -= entropySize;
    ZSTD@version@_refDictContent(dctx, dict, dictSize);
    return 0;
}


]=])
    string(CONFIGURE "${insertion}" insertion @ONLY)
    superzip_zstd_replace_legacy_region(
      "${content}" "static size_t ZSTD${version}_decompress_insertDictionary("
      "size_t ZSTD${version}_decompressBegin_usingDict(" "${insertion}" content)
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish complete identity-bound legacy dictionary repairs. Inputs:
# source_dir owns exact history revisions or the complete final result. Outputs:
# Migrates all shipped versions, rejects unknown bytes and leaves provenance
# archives unchanged; repeated application performs no writes.
function(superzip_patch_zstd_legacy_dictionary source_dir)
  foreach(version IN ITEMS v05 v06 v07)
    set(key "_zstd_legacy_dictionary_${version}")
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual_hash)
    if(actual_hash STREQUAL "${${key}_patched}")
      continue()
    endif()
    if(NOT actual_hash STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    superzip_zstd_rewrite_legacy_dictionary("${content}" "${version}" content)
    superzip_write_verified_zstd_patch("${source}" "${actual_hash}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

set(_zstd_legacy_dictionary_v05_original
    "aad9e79a70142bd9c3dc16973c46c9bd669df883e43fc091cbb3137bdcaf5350")
set(_zstd_legacy_dictionary_v05_patched
    "bd627a804a8fd98dcc580cef65b3f190cac2f01e6136a9d5f0cc485ff36d08aa")
set(_zstd_legacy_dictionary_v06_original
    "9fb2717f459644e888259f03676b0a4bcd86d6dddce483e1d5737d44f1b464ac")
set(_zstd_legacy_dictionary_v06_patched
    "4d8f737c5613cc1abbee283b383e3b97768448f244282511fef502c2b774baed")
set(_zstd_legacy_dictionary_v07_original
    "57c8f43bf5b39908b40e6fe02a2108f99f5dc436644b98f347b754052e0407b1")
set(_zstd_legacy_dictionary_v07_patched
    "2fe88d3ccb50c4c0cf26da2aff6554d67af968f2a40b930ba8ac212f12fe72c8")
