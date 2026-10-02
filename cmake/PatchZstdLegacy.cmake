# Purpose: Apply a pinned dependency transformation only to known source bytes.
# Inputs: path is an extracted source, hashes identify original/patched bytes,
# and content is the complete proposed text. Upstream archives stay immutable.
# Outputs: Atomically replaces the original on an exact match; rejects drift or
# an interrupted/concurrent patch instead of silently overwriting it.
function(superzip_write_verified_zstd_patch path original_hash patched_hash
         content)
  file(SHA256 "${path}" actual_hash)
  if(actual_hash STREQUAL patched_hash)
    return()
  endif()
  if(NOT actual_hash STREQUAL original_hash)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${path}")
  endif()
  string(SHA256 proposed_hash "${content}")
  if(NOT proposed_hash STREQUAL patched_hash)
    message(FATAL_ERROR "Zstandard patch output identity mismatch: ${path}")
  endif()
  set(temporary "${path}.superzip-patch")
  set(raw "${temporary}.raw")
  if(EXISTS "${temporary}" OR EXISTS "${raw}")
    message(FATAL_ERROR "Incomplete or concurrent Zstandard patch: ${path}")
  endif()
  file(WRITE "${raw}" "${content}")
  configure_file("${raw}" "${temporary}" @ONLY NEWLINE_STYLE LF)
  file(SHA256 "${temporary}" written_hash)
  if(NOT written_hash STREQUAL patched_hash)
    message(FATAL_ERROR "Zstandard patch write identity mismatch: ${path}")
  endif()
  file(RENAME "${temporary}" "${path}" RESULT rename_result)
  if(NOT rename_result STREQUAL "0")
    message(FATAL_ERROR "Zstandard patch publication failed: ${rename_result}")
  endif()
  file(REMOVE "${raw}")
endfunction()

# Purpose: Repair v0.5 partial construction without changing the decoder.
# Inputs: source_dir is the extracted v1.5.7 root from the verified archive.
# Outputs: Writes only the identity-checked generated v0.5 source.
function(superzip_patch_zstd_v05 source_dir)
  set(decoder "${source_dir}/lib/legacy/zstd_v05.c")
  file(READ "${decoder}" decoder_content)
  string(
    CONCAT constructor_contract
           "/* Purpose: Construct a fully owned buffered decoder, never a "
           "partial context.\n"
           " * Inputs: None; allocator failure is a normal error path.\n"
           " * Outputs: Returns a complete owner or NULL after releasing "
           "partial allocations. */\n")
  string(
    REPLACE "ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
            "${constructor_contract}ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
            decoder_content "${decoder_content}")
  string(CONCAT checked_create "    zbc->zc = ZSTDv05_createDCtx();\n"
                "    if (zbc->zc==NULL) {\n        free(zbc);\n"
                "        return NULL;\n    }\n")
  string(REPLACE "    zbc->zc = ZSTDv05_createDCtx();\n" "${checked_create}"
                 decoder_content "${decoder_content}")
  superzip_write_verified_zstd_patch(
    "${decoder}"
    "62170472e18505b3347e563cdb1eba439312bb53385663730ac58cd92f3c4678"
    "ee643222919c3a354da6f71d69f55c681ae0647a274384f2143afbe63dd5d9be"
    "${decoder_content}")

endfunction()

# Purpose: Keep shipped legacy context transitions ownership-safe on error.
# Inputs: source_dir is the verified, extracted v1.5.7 source root. Outputs:
# Patches only the generated initializer header with pinned identity.
function(superzip_patch_zstd_initializer source_dir)
  set(header "${source_dir}/lib/legacy/zstd_legacy.h")
  file(READ "${header}" header_content)
  string(
    CONCAT initializer_contract
           "/* Purpose: Initialize a legacy decoder without abandoning its "
           "current owner on failure.\n"
           " * Inputs: legacyContext owns prevVersion; dict is borrowed for "
           "the decoder lifetime.\n"
           " * Outputs: Publishes newVersion only on success; returns "
           "allocation/dictionary errors. */\n")
  string(
    REPLACE "MEM_STATIC size_t ZSTD_initLegacyStream("
            "${initializer_contract}MEM_STATIC size_t ZSTD_initLegacyStream("
            header_content "${header_content}")
  set(release_call "ZSTD_freeLegacyStreamContext(*legacyContext, prevVersion);")
  string(CONCAT early_release "    if (prevVersion != newVersion) "
                "${release_call}\n")
  string(REPLACE "${early_release}" "" header_content "${header_content}")
  string(CONCAT release_existing
                "            if (prevVersion != newVersion) ${release_call}\n")
  string(
    CONCAT unsupported
           "            (void)dict; (void)dictSize;\n"
           "            if (prevVersion != newVersion) {\n"
           "                ${release_call}\n"
           "                *legacyContext = NULL;\n            }\n"
           "            return 0;")
  string(
    REPLACE "            (void)dict; (void)dictSize;\n            return 0;"
            "${unsupported}" header_content "${header_content}")
  set(v04_init
      "            ZBUFFv04_decompressWithDictionary(dctx, dict, dictSize);\n")
  string(REPLACE "${v04_init}" "${v04_init}${release_existing}" header_content
                 "${header_content}")
  foreach(version IN ITEMS 5 6 7)
    string(
      CONCAT create "            ZBUFFv0${version}_DCtx* dctx = "
             "(prevVersion != newVersion) ? ZBUFFv0${version}_createDCtx() : "
             "(ZBUFFv0${version}_DCtx*)*legacyContext;\n")
    string(REPLACE "${create}" "${create}            size_t initResult;\n"
                   header_content "${header_content}")
    string(CONCAT initialize "            ZBUFFv0${version}_"
                  "decompressInitDictionary(dctx, dict, dictSize);\n")
    string(
      CONCAT checked_initialize
             "            initResult = ZBUFFv0${version}_"
             "decompressInitDictionary(dctx, dict, dictSize);\n"
             "            if (ZBUFFv0${version}_isError(initResult)) {\n"
             "                if (prevVersion != newVersion) "
             "ZBUFFv0${version}_freeDCtx(dctx);\n"
             "                return initResult;\n            }\n"
             "${release_existing}")
    string(REPLACE "${initialize}" "${checked_initialize}" header_content
                   "${header_content}")
  endforeach()
  superzip_write_verified_zstd_patch(
    "${header}"
    "90ecc3816d28da0e0537c610fe99209fb9603b1ecdb764bd1228a5226f5c7484"
    "a582b26687409283a230ff99d53cf0ccefa945ec86cf35111d00a3600e8eb848"
    "${header_content}")
endfunction()

# Purpose: Repair custom allocation failure before zero initialization. Inputs:
# source_dir is the verified v1.5.7 source root. Outputs: Writes only the
# hash-checked production allocation helper; preserves upstream notices.
function(superzip_patch_zstd_custom_allocator source_dir)
  set(header "${source_dir}/lib/common/allocations.h")
  file(READ "${header}" content)
  string(
    CONCAT contract
           "/* Purpose: Zero custom storage only after successful allocation.\n"
           " * Inputs: size is the requested extent; customMem supplies "
           "callbacks or the standard allocator.\n"
           " * Outputs: Returns zeroed owned storage or NULL without accessing "
           "failed allocation. */\n")
  string(CONCAT signature "MEM_STATIC void* ZSTD_customCalloc(size_t size, "
                "ZSTD_customMem customMem)\n")
  string(REPLACE "${signature}" "${contract}${signature}" content "${content}")
  string(CONCAT checked_clear "        if (ptr == NULL) {\n"
                "            return NULL;\n        }\n"
                "        ZSTD_memset(ptr, 0, size);\n")
  string(REPLACE "        ZSTD_memset(ptr, 0, size);\n" "${checked_clear}"
                 content "${content}")
  superzip_write_verified_zstd_patch(
    "${header}"
    "6a718e8edaca112abdc0bdfa7de67edefaa44c8d9e514b79672ab58ab3a9118a"
    "b1e3a6f3460a0558727860cd0ade078c2311cc474c6a4aafc161bdf938c296cd"
    "${content}")
endfunction()

# Purpose: Guard a private dependency header without changing its declarations.
# Inputs: header is an extracted source; guard/anchor identify the insertion;
# original_hash/patched_hash pin both complete identities. Outputs: Publishes
# verified generated source or rejects drift/interruption.
function(superzip_patch_zstd_header_guard header guard anchor original_hash
         patched_hash)
  file(READ "${header}" content)
  string(CONCAT guarded_anchor "#ifndef ${guard}\n#define ${guard}\n\n"
                "${anchor}")
  string(REPLACE "${anchor}" "${guarded_anchor}" content "${content}")
  string(APPEND content "\n#endif /* ${guard} */\n")
  superzip_write_verified_zstd_patch("${header}" "${original_hash}"
                                     "${patched_hash}" "${content}")
endfunction()

# Purpose: Apply the production dependency's source repairs reproducibly.
# Inputs: source_dir is the extracted root of the verified upstream archive.
# Outputs: Patches generated files or fails closed; leaves provenance untouched.
function(superzip_patch_zstd_legacy source_dir)
  superzip_patch_zstd_v05("${source_dir}")
  superzip_patch_zstd_initializer("${source_dir}")
  superzip_patch_zstd_custom_allocator("${source_dir}")
  superzip_patch_zstd_header_guard(
    "${source_dir}/lib/dictBuilder/cover.h"
    ZSTD_COVER_H
    "#ifndef ZDICT_STATIC_LINKING_ONLY\n"
    "6e2906e7e5c486a5b7f479f60210ff67d068b2c58744ad6a936ba5a9f4a23755"
    "432323ee0a02dbfb49abb146137fc152142df20b4223e1a8a5f6f6303a8eeed5")
  superzip_patch_zstd_header_guard(
    "${source_dir}/lib/compress/hist.h"
    ZSTD_HIST_H
    "/* --- dependencies --- */\n"
    "9e3363e69d5fa35c1f6e8d2970c6c2453c06fd8a8ab3cc516a14c77d07cdea35"
    "982484a96936e53fcca4c179e522fcb253608c93c2301ca636a3b29e1128b099")
endfunction()
