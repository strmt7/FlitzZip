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
  file(SHA256 "${decoder}" current_hash)
  if(current_hash STREQUAL
     "dd60a43788f2a2150ae9ddf91a43ea34c6a03d6a420196e209f5aca2ffb217e2")
    return()
  endif()
  file(READ "${decoder}" decoder_content)
  if(current_hash STREQUAL
     "62170472e18505b3347e563cdb1eba439312bb53385663730ac58cd92f3c4678")
    string(
      CONCAT constructor_contract
             "/* Purpose: Construct a fully owned buffered decoder, never a "
             "partial context.\n"
             " * Inputs: None; allocator failure is a normal error path.\n"
             " * Outputs: Returns a complete owner or NULL after releasing "
             "partial allocations. */\n")
    string(
      REPLACE
        "ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
        "${constructor_contract}ZBUFFv05_DCtx* ZBUFFv05_createDCtx(void)\n"
        decoder_content "${decoder_content}")
    string(CONCAT checked_create "    zbc->zc = ZSTDv05_createDCtx();\n"
                  "    if (zbc->zc==NULL) {\n        free(zbc);\n"
                  "        return NULL;\n    }\n")
    string(REPLACE "    zbc->zc = ZSTDv05_createDCtx();\n" "${checked_create}"
                   decoder_content "${decoder_content}")
  endif()
  string(REPLACE [=[if (offsetCode | !litLength)]=]
                 [=[if (offsetCode != 0 || litLength == 0)]=] decoder_content
                 "${decoder_content}")
  string(
    CONCAT patch_fragment_1
           "/* Purpose: Decode one legacy sequence and maintain rep"
           "eat-offset history.\n"
           " * Inputs: seq receives output; seqState owns validated"
           " entropy states and borrows the bitstream.\n"
           " * Outputs: Writes lengths and offset and advances deco"
           "der state without changing wire semantics. */\n"
           "static void ZSTDv05_decodeSequence(")
  string(REPLACE [=[static void ZSTDv05_decodeSequence(]=]
                 "${patch_fragment_1}" decoder_content "${decoder_content}")
  file(SHA256 "${decoder}" current_hash)
  set(original_hash
      "62170472e18505b3347e563cdb1eba439312bb53385663730ac58cd92f3c4678")
  # Accept only the exact earlier constructor repair for incremental builds.
  if(current_hash STREQUAL
     "ee643222919c3a354da6f71d69f55c681ae0647a274384f2143afbe63dd5d9be")
    set(original_hash "${current_hash}")
  endif()
  superzip_write_verified_zstd_patch(
    "${decoder}" "${original_hash}"
    "dd60a43788f2a2150ae9ddf91a43ea34c6a03d6a420196e209f5aca2ffb217e2"
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

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_cover_cleanup source_dir)
  set(source "${source_dir}/lib/dictBuilder/cover.c")
  file(READ "${source}" content)
  string(CONCAT patch_fragment_2 "  if (map->data) {\n"
                "    free(map->data);\n" "  }")
  string(REPLACE "${patch_fragment_2}" [=[  free(map->data);]=] content
                 "${content}")
  string(CONCAT patch_fragment_3 "  if (dst) {\n" "    free(dst);\n" "  }")
  string(REPLACE "${patch_fragment_3}" [=[  free(dst);]=] content "${content}")
  string(CONCAT patch_fragment_4 "  if (best->dict) {\n"
                "    free(best->dict);\n" "  }")
  string(REPLACE "${patch_fragment_4}" [=[  free(best->dict);]=] content
                 "${content}")
  string(CONCAT patch_fragment_5 "        if (best->dict) {\n"
                "          free(best->dict);\n" "        }")
  string(REPLACE "${patch_fragment_5}" [=[        free(best->dict);]=] content
                 "${content}")
  string(
    CONCAT patch_fragment_6
           "/* Purpose: Release map-owned storage and reset the map"
           ".\n"
           " * Inputs: map points to an initialized map; its data m"
           "ay be NULL.\n"
           " * Outputs: Releases data and clears size and ownership"
           ". */\n"
           "static void COVER_map_destroy(")
  string(REPLACE [=[static void COVER_map_destroy(]=] "${patch_fragment_6}"
                 content "${content}")
  string(
    CONCAT patch_fragment_7
           "/* Purpose: Measure sample compression using the candid"
           "ate dictionary.\n"
           " * Inputs: parameters, sample sizes/bytes/offsets and d"
           "ictionary extents are validated by the caller.\n"
           " * Outputs: Returns total compressed size or an error; "
           "releases temporary storage. */\n"
           "size_t COVER_checkTotalCompressedSize(")
  string(REPLACE [=[size_t COVER_checkTotalCompressedSize(]=]
                 "${patch_fragment_7}" content "${content}")
  string(
    CONCAT patch_fragment_8
           "/* Purpose: Wait for jobs and release optimizer-owned r"
           "esources.\n"
           " * Inputs: best is NULL or initialized shared state wit"
           "h counted live jobs.\n"
           " * Outputs: Joins logical completion and releases dicti"
           "onary and synchronization state. */\n"
           "void COVER_best_destroy(")
  string(REPLACE [=[void COVER_best_destroy(]=] "${patch_fragment_8}" content
                 "${content}")
  string(
    CONCAT patch_fragment_9
           "/* Purpose: Record worker completion and update the bes"
           "t dictionary under lock.\n"
           " * Inputs: best is NULL or initialized state; parameter"
           "s and selection describe one completed job.\n"
           " * Outputs: Decrements live jobs, updates owned diction"
           "ary and signals completion. */\n"
           "void COVER_best_finish(")
  string(REPLACE [=[void COVER_best_finish(]=] "${patch_fragment_9}" content
                 "${content}")
  superzip_write_verified_zstd_patch(
    "${source}"
    "2419631b20b0f4867d0f3c178fc4a3006d05f58e8ce3ce6657133f85ed40d683"
    "89a86e4217d306fd236bfb41cbdb7081e13082d88acc0f780a713df46da3b0da"
    "${content}")
endfunction()

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_worker_condition source_dir)
  set(source "${source_dir}/lib/compress/zstdmt_compress.c")
  file(READ "${source}" content)
  string(CONCAT patch_fragment_10
                "if (!mtctx->factory | !mtctx->jobs | !mtctx->bufPool | "
                "!mtctx->cctxPool | !mtctx->seqPool | initError)")
  string(CONCAT patch_fragment_11
                "if (!mtctx->factory || !mtctx->jobs || !mtctx->bufPool "
                "|| !mtctx->cctxPool || !mtctx->seqPool || initError)")
  string(REPLACE "${patch_fragment_10}" "${patch_fragment_11}" content
                 "${content}")
  string(
    CONCAT patch_fragment_12
           "/* Purpose: Construct a complete multithreaded compress"
           "ion owner.\n"
           " * Inputs: nbWorkers is validated; cMem supplies alloca"
           "tion callbacks; pool is optional borrowed state.\n"
           " * Outputs: Returns a complete context or NULL after re"
           "leasing partial allocations. */\n"
           "MEM_STATIC ZSTDMT_CCtx* ZSTDMT_createCCtx_advanced_inte"
           "rnal(")
  string(
    REPLACE [=[MEM_STATIC ZSTDMT_CCtx* ZSTDMT_createCCtx_advanced_internal(]=]
            "${patch_fragment_12}" content "${content}")
  superzip_write_verified_zstd_patch(
    "${source}"
    "c83db699b4041bf4db89c5558db490dd295635f1eeefd60b643fbc862bbac7b5"
    "0e2910244eedc12f7728f59e232325e964c2fc546f87f4bf74881016c47cf21a"
    "${content}")
endfunction()

# Purpose: Clarify equivalent cleanup or logical conditions in pinned source.
# Inputs: source_dir is the verified extracted v1.5.7 dependency root. Outputs:
# Publishes exact reviewed bytes; rejects source drift and preserves provenance.
function(superzip_patch_zstd_v04_condition source_dir)
  set(source "${source_dir}/lib/legacy/zstd_v04.c")
  file(READ "${source}" content)
  string(REPLACE [=[if (offsetCode | !litLength)]=]
                 [=[if (offsetCode != 0 || litLength == 0)]=] content
                 "${content}")
  string(
    CONCAT patch_fragment_13
           "/* Purpose: Decode one legacy sequence and maintain rep"
           "eat-offset history.\n"
           " * Inputs: seq receives output; seqState owns validated"
           " entropy states and borrows the bitstream.\n"
           " * Outputs: Writes lengths and offset and advances deco"
           "der state without changing wire semantics. */\n"
           "static void ZSTD_decodeSequence(")
  string(REPLACE [=[static void ZSTD_decodeSequence(]=] "${patch_fragment_13}"
                 content "${content}")
  superzip_write_verified_zstd_patch(
    "${source}"
    "a80d9591ff3fc387a05e8e075713af9ad34596dc5c8681666346a716013a6e14"
    "079df3416c7aa806e7e9bb3a2db8c4c65dbe938b0b7e261d7b66591e0e29b551"
    "${content}")
endfunction()

# Purpose: Apply the production dependency's source repairs reproducibly.
# Inputs: source_dir is the extracted root of the verified upstream archive.
# Outputs: Patches generated files or fails closed; leaves provenance untouched.
function(superzip_patch_zstd_legacy source_dir)
  superzip_patch_zstd_v05("${source_dir}")
  superzip_patch_zstd_cover_cleanup("${source_dir}")
  superzip_patch_zstd_worker_condition("${source_dir}")
  superzip_patch_zstd_v04_condition("${source_dir}")
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
