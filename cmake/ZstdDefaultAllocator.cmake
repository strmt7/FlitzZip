# One immutable default allocator serves all library translation units. Exact
# input/output identities preserve provenance and cache migration.
set(_zstd_default_allocator_static_original
    "58f72d1bbfa9d1a2349d7c6077a0414c8b6f56624eef4900816b067344d2733d")
set(_zstd_default_allocator_static_patched
    "8cee9bec299b27ad672bf717480a01a4ece2a6b3b8424508a0bb1ad1c1ddb257")
set(_zstd_default_allocator_common_original
    "f49eb8023a90d3de925373bd9ee0d36ed46b26c529840b64220397446cd73795")
set(_zstd_default_allocator_common_patched
    "a49b72d23e445011c18a3859a02d18fd649ec4dfe1f5f5c78550d97ae0bd99ae")

# Purpose: Replace per-translation-unit default storage with one API
# declaration. Inputs: Exact original static API text. Outputs: Same type, name
# and linkage contract, with the definition supplied by the common library
# translation unit.
function(superzip_zstd_share_default_allocator content output)
  set(original
      [=[static
#ifdef __GNUC__
__attribute__((__unused__))
#endif

#if defined(__clang__) && __clang_major__ >= 5
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
]=])
  string(
    APPEND
    original
    "ZSTD_customMem const ZSTD_defaultCMem = { NULL, NULL, NULL };"
    "  /**< this constant defers to stdlib's functions */\n"
    [=[
#if defined(__clang__) && __clang_major__ >= 5
#pragma clang diagnostic pop
#endif]=])
  set(replacement
      [=[ZSTDLIB_STATIC_API extern ZSTD_customMem const ZSTD_defaultCMem;
/**< One shared immutable value defers to the standard allocator. */]=])
  string(FIND "${content}" "${original}" location)
  if(location LESS 0)
    message(FATAL_ERROR "Zstandard default allocator declaration mismatch")
  endif()
  string(REPLACE "${original}" "${replacement}" content "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Migrate only the exact previously published static header component.
# Inputs: Component path and its actual digest. Outputs: Atomically publishes
# shared storage declaration; all unknown component bytes remain rejected.
function(superzip_zstd_migrate_default_allocator path actual)
  if(NOT actual STREQUAL _zstd_default_allocator_static_original)
    return()
  endif()
  file(READ "${path}" content)
  superzip_zstd_share_default_allocator("${content}" content)
  superzip_write_verified_zstd_patch(
    "${path}" "${actual}" "${_zstd_default_allocator_static_patched}"
    "${content}")
endfunction()

# Purpose: Define the shared immutable allocator in the production common unit.
# Inputs: Exact upstream or repaired zstd_common.c. Outputs: Same null callback
# values and allocator semantics, with one definition for all C/C++ callers.
function(superzip_patch_zstd_default_allocator source_dir)
  set(source "${source_dir}/lib/common/zstd_common.c")
  file(SHA256 "${source}" actual)
  if(actual STREQUAL _zstd_default_allocator_common_patched)
    return()
  endif()
  if(NOT actual STREQUAL _zstd_default_allocator_common_original)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  string(
    CONCAT definition
           "#include \"zstd_internal.h\"\n\n"
           "/* One immutable allocator value is shared by all "
           "library translation units. */\n"
           "ZSTD_customMem const ZSTD_defaultCMem = { NULL, NULL, NULL };\n")
  string(REPLACE "#include \"zstd_internal.h\"\n" "${definition}" content
                 "${content}")
  superzip_write_verified_zstd_patch(
    "${source}" "${actual}" "${_zstd_default_allocator_common_patched}"
    "${content}")
endfunction()
