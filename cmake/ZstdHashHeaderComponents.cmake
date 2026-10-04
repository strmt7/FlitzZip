# Purpose: Separate hash configuration, public API, optional state and
# implementation into guarded components. Inputs: Exact predecessor text.
# Outputs: Proposed components and dispatcher; retains late namespace changes
# and the original implementation opt-in sequence.
function(superzip_split_zstd_hash_header content key)
  string(FIND "${content}" "#ifndef ${${key}_public_guard}" public_begin)
  string(FIND "${content}" "#endif${${key}_public_end}" public_end)
  string(LENGTH "#endif${${key}_public_end}" public_end_length)
  math(EXPR public_end "${public_end} + ${public_end_length}")
  string(FIND "${content}" "${${key}_static_condition}" static_begin)
  string(FIND "${content}" "${${key}_static_end}" static_end)
  string(LENGTH "${${key}_static_end}" static_end_length)
  math(EXPR static_end "${static_end} + ${static_end_length}")
  if(public_begin LESS 0
     OR public_end LESS_EQUAL public_begin
     OR static_begin LESS public_end
     OR static_end LESS_EQUAL static_begin)
    message(FATAL_ERROR "Zstandard hash header component boundary mismatch")
  endif()
  string(FIND "${content}" "/* Local adaptations for Zstandard */" license_end)
  if(license_end LESS 0)
    message(FATAL_ERROR "Zstandard hash license boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${license_end} license)
  string(SUBSTRING "${content}" 0 ${public_begin} configuration)
  math(EXPR public_length "${public_end} - ${public_begin}")
  string(SUBSTRING "${content}" ${public_begin} ${public_length} public)
  math(EXPR static_length "${static_end} - ${public_end}")
  string(SUBSTRING "${content}" ${public_end} ${static_length} static)
  string(SUBSTRING "${content}" ${static_end} -1 implementation)
  string(REPLACE "${${key}_static_condition}" "#ifndef ${${key}_static_guard}"
                 static "${static}")
  string(FIND "${implementation}" "${${key}_implementation_condition}" begin)
  if(begin LESS 0)
    message(FATAL_ERROR "Zstandard hash implementation boundary mismatch")
  endif()
  string(REPLACE "${${key}_implementation_condition}"
                 "#ifndef ${${key}_implementation_guard}" implementation
                 "${implementation}")
  foreach(part IN ITEMS public static implementation)
    set(${part} "${license}${${part}}\n")
    set(${part}
        "${${part}}"
        PARENT_SCOPE)
  endforeach()
  string(
    CONCAT dispatch
           "${configuration}"
           "/* Preserve staged namespace and implementation opt-in. */\n"
           "#include \"xxhash_public.h\"\n"
           "#if defined(XXH_STATIC_LINKING_ONLY)\n"
           "#include \"xxhash_static.h\"\n#endif\n"
           "#if defined(XXH_INLINE_ALL) || defined(XXH_PRIVATE_API) "
           "|| defined(XXH_IMPLEMENTATION)\n"
           "#include \"xxhash_implementation.h\"\n#endif\n")
  set(dispatch
      "${dispatch}"
      PARENT_SCOPE)
endfunction()

set(_zstd_header_component_xxhash_path "lib/common/xxhash.h")
set(_zstd_header_component_xxhash_stem "xxhash")
set(_zstd_header_component_xxhash_original
    "327a38e032aa6daccd0145fce664d6c65d001290955fd9c9d47aedd6fcc9cb41")
set(_zstd_header_component_xxhash_public_guard "XXHASH_H_5627135585666179")
set(_zstd_header_component_xxhash_public_end " /* XXHASH_H_5627135585666179 */")
set(_zstd_header_component_xxhash_static_guard "XXHASH_H_STATIC_13879238742")
set(_zstd_header_component_xxhash_implementation_guard "XXH_IMPLEM_13a8737387")
string(CONCAT _zstd_header_component_xxhash_static_condition
              "#if defined(XXH_STATIC_LINKING_ONLY) "
              "&& !defined(XXHASH_H_STATIC_13879238742)")
string(CONCAT _zstd_header_component_xxhash_static_end
              "#endif  /* defined(XXH_STATIC_LINKING_ONLY) "
              "&& !defined(XXHASH_H_STATIC_13879238742) */")
string(
  CONCAT _zstd_header_component_xxhash_implementation_condition
         "#if ( defined(XXH_INLINE_ALL) || defined(XXH_PRIVATE_API) "
         "\\\n   || defined(XXH_IMPLEMENTATION) ) "
         "&& !defined(XXH_IMPLEM_13a8737387)")
set(_zstd_header_component_xxhash_patched
    "8a8aa3325b1c78b4fc3b02bd29ff173f97f5f08518a682ef028dd0cd56b3ad82")
set(_zstd_header_component_xxhash_public_hash
    "c59f7c221de7fc81e4ad8bd846fa4ad0a6aa9786d9d6b171597aaf4192df8d3b")
set(_zstd_header_component_xxhash_static_hash
    "ddbb9277eaae16da08df87b21d4a75ec30aa4b0515d69738bdbd0201d8750ccf")
set(_zstd_header_component_xxhash_implementation_hash
    "012b2e525a28b1345d0c0b636c01a40b9c8a9b99188a8f1fd6161142eeb52072")
