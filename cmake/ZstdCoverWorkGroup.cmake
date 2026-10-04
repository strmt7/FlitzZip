# Purpose: Replace borrowed optimizer stack state with one joined heap owner.
# Inputs: Exact predecessor text and optimizer flavor. Outputs: Proposed
# complete source; adjacent trainer implementations retain their existing
# contracts.
function(superzip_rewrite_zstd_work_group content flavor output)
  if(flavor STREQUAL cover)
    string(
      CONCAT old_geometry "  if ((nbSamples != 0 && samplesSizes == NULL) ||\n"
             "      nbSamples > (size_t)PTRDIFF_MAX / sizeof(size_t)) "
             "return (size_t)-1;")
    string(
      CONCAT
        checked_geometry
        "  if (nbSamples != 0 && samplesSizes == NULL) "
        "return (size_t)-1;\n"
        "#if PTRDIFF_MAX <= UINT32_MAX\n"
        "  /* Wider pointer extents represent every unsigned sample count. */\n"
        "  if (nbSamples > (size_t)PTRDIFF_MAX / sizeof(size_t)) "
        "return (size_t)-1;\n#endif")
    string(REPLACE "${old_geometry}" "${checked_geometry}" content "${content}")
  endif()
  set(indent "  ")
  set(context COVER)
  set(signature
      "ZDICTLIB_STATIC_API size_t ZDICT_optimizeTrainFromBuffer_cover(")
  if(flavor STREQUAL fastcover)
    set(indent "    ")
    set(context FASTCOVER)
    string(CONCAT signature "ZDICTLIB_STATIC_API size_t\n"
                  "ZDICT_optimizeTrainFromBuffer_fastCover(")
  endif()
  string(FIND "${content}" "${signature}" begin)
  if(begin LESS 0)
    message(FATAL_ERROR "Zstandard optimizer ownership boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${begin} -1 region)
  string(
    CONCAT pool_original
           "${indent}if (nbThreads > 1) {\n"
           "${indent}  pool = POOL_create(nbThreads, 1);\n"
           "${indent}  if (!pool) {\n"
           "${indent}    return ERROR(memory_allocation);\n"
           "${indent}  }\n${indent}}")
  string(
    CONCAT pool_owned
           "${indent}group = COVER_createWorkGroup(nbThreads);\n"
           "${indent}if (group == NULL) return ERROR(memory_allocation);\n"
           "${indent}best = COVER_workGroupBest(group);\n"
           "${indent}pool = COVER_workGroupPool(group);")
  string(REPLACE "${pool_original}" "${pool_owned}" region "${region}")
  string(REPLACE "COVER_best_t best;"
                 "COVER_workGroup* group;\n${indent}COVER_best_t* best;" region
                 "${region}")
  string(REPLACE "${indent}COVER_best_init(&best);\n" "" region "${region}")
  string(
    CONCAT
      context_owned
      "${context}_ctx_t* ctx = (${context}_ctx_t*)COVER_prepareWorkContext(\n"
      "${indent}    group, sizeof(${context}_ctx_t), "
      "ZSTD_ALIGNOF(${context}_ctx_t), ${context}_destroyWorkContext);\n"
      "${indent}  if (ctx == NULL) {\n"
      "${indent}    COVER_releaseWorkGroup(group);\n"
      "${indent}    return ERROR(memory_allocation);\n${indent}  }")
  string(REPLACE "${context}_ctx_t ctx;" "${context_owned}" region "${region}")
  string(REPLACE "COVER_best_destroy(&best);" "" region "${region}")
  string(REPLACE "${context}_ctx_destroy(&ctx);"
                 "COVER_finishWorkContext(group);" region "${region}")
  string(REPLACE "POOL_free(pool);" "COVER_releaseWorkGroup(group);" region
                 "${region}")
  string(REPLACE "COVER_best_wait(&best);" "" region "${region}")
  string(REPLACE "if (!warned)"
                 "COVER_commitWorkContext(group);\n${indent}  if (!warned)"
                 region "${region}")
  string(REPLACE "const size_t dictSize = best.dictSize;" "" region "${region}")
  string(CONCAT copy "const size_t dictSize = COVER_copyWorkDictionary(\n"
                "${indent}    group, dictBuffer, dictBufferCapacity);")
  string(REPLACE "memcpy(dictBuffer, best.dict, dictSize);" "${copy}" region
                 "${region}")
  foreach(replacement IN ITEMS ctx best)
    string(REPLACE "&${replacement}" "${replacement}" region "${region}")
    string(REPLACE "${replacement}." "${replacement}->" region "${region}")
  endforeach()
  string(REGEX REPLACE "\n[ \t]+\n" "\n" region "${region}")
  string(
    CONCAT wrapper
           "/* Purpose: Destroy the initialized private C optimizer context.\n"
           " * Inputs: Context committed by its owner after successful "
           "initialization.\n"
           " * Outputs: Matching C child cleanup after all workers have "
           "returned. */\n"
           "static void ${context}_destroyWorkContext(void* context) {\n"
           "  ${context}_ctx_destroy((${context}_ctx_t*)context);\n}\n\n")
  string(
    CONCAT contract
           "/* Purpose: Optimize dictionary parameters with explicitly "
           "owned worker state.\n"
           " * Inputs: Live sample/output allocations and parameter records "
           "under the public C API.\n"
           " * Outputs: Dictionary size or error; all workers return before "
           "shared state is released. */\n")
  set(${output}
      "${prefix}${wrapper}${contract}${region}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish explicit lifetime ownership for both exported optimizers.
# Inputs: Extracted exact patched dictionary sources. Outputs: Complete known
# revisions or identity rejection; immutable upstream archives are preserved.
function(superzip_patch_zstd_work_group source_dir)
  foreach(flavor IN ITEMS cover fastcover)
    set(key "_zstd_work_group_${flavor}")
    set(source "${source_dir}/lib/dictBuilder/${flavor}.c")
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${${key}_patched}")
      continue()
    endif()
    if(NOT actual STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    superzip_rewrite_zstd_work_group("${content}" "${flavor}" content)
    string(CONCAT include "#include \"ZstdCoverWorkGroup.h\"\n")
    string(REPLACE "#include \"cover.h\"" "#include \"cover.h\"\n${include}"
                   content "${content}")
    superzip_write_verified_zstd_patch("${source}" "${actual}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

set(_zstd_work_group_cover_original
    "ccbb084aac45ecf3d31e15e1e0a2102efe8e429e8e3fada2afd9ed728946bb02")
set(_zstd_work_group_fastcover_original
    "ae43f56fd892fe02ce5fe048980a2ae2c63eadc6e70b8e3662fe56d08275e085")
set(_zstd_work_group_cover_patched
    "4e460da9bb4ce92b41925fd3ad36e6bfdc03c0fe4ffe9f4069223b7c1e2d334e")
set(_zstd_work_group_fastcover_patched
    "3fd1042d41da13e7330deb0134a757791d492fe9e8d5d9d1f101a23cddb81c9a")
