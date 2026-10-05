# Purpose: Split public and optional declarations into independently guarded
# phases. Inputs: Exact staged header bytes and metadata key. Outputs: Three
# proposed texts; preserves namespace, language linkage, macros and late opt-in
# behavior.
function(superzip_split_zstd_header content key)
  string(FIND "${content}" "#ifndef ${${key}_public_guard}" public_begin)
  string(FIND "${content}" "#endif${${key}_public_end}" public_end)
  if(public_begin LESS 0 OR public_end LESS_EQUAL public_begin)
    message(FATAL_ERROR "Zstandard header component boundary mismatch")
  endif()
  string(LENGTH "#endif${${key}_public_end}" end_length)
  math(EXPR public_end "${public_end} + ${end_length}")
  string(SUBSTRING "${content}" 0 ${public_begin} license)
  string(SUBSTRING "${content}" 0 ${public_end} public)
  string(SUBSTRING "${content}" ${public_end} -1 static)
  string(FIND "${static}" "${${key}_static_condition}" static_begin)
  if(static_begin LESS 0)
    message(FATAL_ERROR "Zstandard static header boundary mismatch")
  endif()
  string(REGEX REPLACE "^\n+" "" static "${static}")
  string(REPLACE "${${key}_static_condition}" "#ifndef ${${key}_static_guard}"
                 static "${static}")
  set(static "${license}${static}")
  if(key STREQUAL "_zstd_header_component_zstd")
    superzip_zstd_share_default_allocator("${static}" static)
  endif()
  set(public "${public}\n")
  string(
    CONCAT dispatch
           "${license}"
           "/* Dispatch independent guarded API phases; "
           "late opt-in remains supported. */\n"
           "#include \"${${key}_stem}_public.h\"\n"
           "#if defined(${${key}_opt_in})\n"
           "#include \"${${key}_stem}_static.h\"\n#endif\n")
  foreach(part IN ITEMS public static dispatch)
    set(${part}
        "${${part}}"
        PARENT_SCOPE)
  endforeach()
endfunction()

# Purpose: Atomically publish a derived header only under its exact reviewed
# identity. Inputs: Absent or matching component path, expected digest and
# complete text. Outputs: Matching LF source or rejection; never overwrites
# unknown components.
function(superzip_publish_zstd_header_component path expected content)
  string(SHA256 proposed "${content}")
  if(NOT proposed STREQUAL expected)
    message(FATAL_ERROR "Zstandard header output identity mismatch: ${path}; "
                        "observed ${proposed}")
  endif()
  if(EXISTS "${path}")
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
      message(
        FATAL_ERROR "Zstandard header component identity mismatch: ${path}")
    endif()
    return()
  endif()
  set(temporary "${path}.superzip-component")
  set(raw "${temporary}.raw")
  if(EXISTS "${temporary}" OR EXISTS "${raw}")
    message(FATAL_ERROR "Incomplete or concurrent Zstandard header: ${path}")
  endif()
  file(WRITE "${raw}" "${content}")
  configure_file("${raw}" "${temporary}" @ONLY NEWLINE_STYLE LF)
  file(SHA256 "${temporary}" written)
  if(NOT written STREQUAL expected OR EXISTS "${path}")
    message(
      FATAL_ERROR "Zstandard header publication identity mismatch: ${path}")
  endif()
  file(RENAME "${temporary}" "${path}" RESULT result)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Zstandard header publication failed: ${result}")
  endif()
  file(REMOVE "${raw}")
endfunction()

# Purpose: Publish all header phases while preserving the public dispatcher
# interface. Inputs: Exact predecessor or complete generated dispatcher and
# components. Outputs: Verified component graph; every unknown, missing or
# altered output fails closed, including repeated configuration of already
# dispatched headers.
function(superzip_patch_zstd_header_components source_dir)
  foreach(flavor IN ITEMS zstd zdict fse xxhash)
    set(key "_zstd_header_component_${flavor}")
    set(parts public static)
    if(flavor STREQUAL "xxhash")
      list(APPEND parts implementation)
    endif()
    set(source "${source_dir}/${${key}_path}")
    get_filename_component(directory "${source}" DIRECTORY)
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${${key}_patched}")
      foreach(part IN LISTS parts)
        set(component "${directory}/${${key}_stem}_${part}.h")
        if(NOT EXISTS "${component}")
          message(
            FATAL_ERROR "Missing Zstandard header component: ${component}")
        endif()
        file(SHA256 "${component}" component_hash)
        if(flavor STREQUAL "zstd" AND part STREQUAL "static")
          superzip_zstd_migrate_default_allocator("${component}"
                                                  "${component_hash}")
          file(SHA256 "${component}" component_hash)
        endif()
        if(NOT component_hash STREQUAL "${${key}_${part}_hash}")
          message(
            FATAL_ERROR
              "Zstandard header component identity mismatch: ${component}")
        endif()
      endforeach()
      continue()
    endif()
    if(NOT actual STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    if(flavor STREQUAL "xxhash")
      superzip_split_zstd_hash_header("${content}" "${key}")
    else()
      superzip_split_zstd_header("${content}" "${key}")
    endif()
    foreach(part IN LISTS parts)
      superzip_publish_zstd_header_component(
        "${directory}/${${key}_stem}_${part}.h" "${${key}_${part}_hash}"
        "${${part}}")
    endforeach()
    superzip_write_verified_zstd_patch("${source}" "${actual}"
                                       "${${key}_patched}" "${dispatch}")
  endforeach()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/ZstdHashHeaderComponents.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ZstdDefaultAllocator.cmake")

set(_zstd_header_component_zstd_path "lib/zstd.h")
set(_zstd_header_component_zstd_stem "zstd")
set(_zstd_header_component_zstd_public_guard "ZSTD_H_235446")
set(_zstd_header_component_zstd_public_end "  /* ZSTD_H_235446 */")
set(_zstd_header_component_zstd_static_guard "ZSTD_H_ZSTD_STATIC_LINKING_ONLY")
set(_zstd_header_component_zstd_opt_in "ZSTD_STATIC_LINKING_ONLY")
string(CONCAT _zstd_header_component_zstd_static_condition
              "#if defined(ZSTD_STATIC_LINKING_ONLY) "
              "&& !defined(ZSTD_H_ZSTD_STATIC_LINKING_ONLY)")
set(_zstd_header_component_zstd_original
    "9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b")
set(_zstd_header_component_zdict_path "lib/zdict.h")
set(_zstd_header_component_zdict_stem "zdict")
set(_zstd_header_component_zdict_public_guard "ZSTD_ZDICT_H")
set(_zstd_header_component_zdict_public_end "   /* ZSTD_ZDICT_H */")
set(_zstd_header_component_zdict_static_guard "ZSTD_ZDICT_H_STATIC")
set(_zstd_header_component_zdict_opt_in "ZDICT_STATIC_LINKING_ONLY")
string(CONCAT _zstd_header_component_zdict_static_condition
              "#if defined(ZDICT_STATIC_LINKING_ONLY) "
              "&& !defined(ZSTD_ZDICT_H_STATIC)")
set(_zstd_header_component_zdict_original
    "65f365e97b6c7b4ba47f1dfa0d76c9ab5fce62ce8c5394390d76c2136c1d1384")
set(_zstd_header_component_fse_path "lib/common/fse.h")
set(_zstd_header_component_fse_stem "fse")
set(_zstd_header_component_fse_public_guard "FSE_H")
set(_zstd_header_component_fse_public_end "  /* FSE_H */")
set(_zstd_header_component_fse_static_guard "FSE_H_FSE_STATIC_LINKING_ONLY")
set(_zstd_header_component_fse_opt_in "FSE_STATIC_LINKING_ONLY")
string(CONCAT _zstd_header_component_fse_static_condition
              "#if defined(FSE_STATIC_LINKING_ONLY) "
              "&& !defined(FSE_H_FSE_STATIC_LINKING_ONLY)")
set(_zstd_header_component_fse_original
    "7e861d1f104b3bdb2e4a7a14e7e44418b7a05124c712d84d5d7255a8f4b32117")
set(_zstd_header_component_zstd_patched
    "a41bec0ee6606af5fcfb4e4cd0bcbe7c5f110c9ac2321a398f5eaae024217287")
set(_zstd_header_component_zstd_public_hash
    "575c4cf696d71c37bd0ed865c28ea85ead4bfbb8518dcf4ca8a41d6f8718f785")
set(_zstd_header_component_zstd_static_hash
    "${_zstd_default_allocator_static_patched}")
set(_zstd_header_component_zdict_patched
    "5bf49a85d91ef56074709802549be7e113a1acf11fb3ad39313037c08959f810")
set(_zstd_header_component_zdict_public_hash
    "b3a346725144d7cbe26d7405b3ab3b092c0cd6df19cd66246cb071c5e17b200c")
set(_zstd_header_component_zdict_static_hash
    "70dbab01e67aab138d1112ed268133116ab5588f4729135a16e0badf6a566d7e")
set(_zstd_header_component_fse_patched
    "6c93cfb77f3f6d78e4caa097729abbc82f376d4bc219b3b5e3619bf723fc39a5")
set(_zstd_header_component_fse_public_hash
    "67f067d5862d735f26ef9f23e628eebcb93c4f5334a97ace73b04bee6a3eccee")
set(_zstd_header_component_fse_static_hash
    "f3d7f1ad0fa9d2e7de135883bd5cb20a0d9a2b666aeb02eb17567a563ceab1d4")
