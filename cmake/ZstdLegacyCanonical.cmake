# Generate complete owned decoders directly from immutable upstream provenance.
# Historical identities describe migration inputs, never scanner dispositions.
include("${CMAKE_CURRENT_LIST_DIR}/ZstdLegacyCanonicalHashes.cmake")

# Purpose: Recognize only complete previously published decoder revisions.
# Inputs: One shipped version and an actual whole-file SHA-256. Outputs: True
# for a pinned pristine or historical revision; false for drift.
function(superzip_zstd_canonical_input version actual output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyHistory.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedBuffers.cmake")
  include(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedDecoderHashes.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdTableGeometry.cmake")
  set(known "${_zstd_canonical_${version}_pristine}")
  foreach(part IN ITEMS literals stream history dictionary owned)
    foreach(stage IN ITEMS original patched preceding prior empty_prior)
      list(APPEND known "${_zstd_legacy_${part}_${version}_${stage}}")
    endforeach()
  endforeach()
  list(APPEND known "${_zstd_owned_decoder_${version}_original}"
       "${_zstd_owned_decoder_${version}_patched}")
  if(version STREQUAL "v05")
    list(APPEND known
         "ee643222919c3a354da6f71d69f55c681ae0647a274384f2143afbe63dd5d9be")
  elseif(version STREQUAL "v07")
    list(APPEND known "${_zstd_table_geometry_legacy_patched}")
  endif()
  if(actual IN_LIST known)
    set(${output}
        TRUE
        PARENT_SCOPE)
  else()
    set(${output}
        FALSE
        PARENT_SCOPE)
  endif()
endfunction()

# Purpose: Preserve reviewed foundational annotations before direct generation.
# Inputs: Exact pristine text after the v05 constructor/sequence repair.
# Outputs: The exact historical foundation text, checked against its identity.
function(superzip_zstd_canonical_foundation content version output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCommentClarifications.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals.cmake")
  foreach(index RANGE 0 16 1)
    set(key "_zstd_comment_${index}")
    if("${${key}_path}" STREQUAL "lib/legacy/zstd_${version}.c")
      math(EXPR last "${${key}_count} - 1")
      foreach(item RANGE 0 ${last} 1)
        set(patch "${key}_${item}")
        string(REPLACE "${${patch}_old}" "${${patch}_new}" content "${content}")
      endforeach()
    endif()
  endforeach()
  string(SHA256 actual "${content}")
  if(NOT actual STREQUAL "${_zstd_legacy_literals_${version}_original}")
    message(FATAL_ERROR "Zstandard canonical foundation identity mismatch")
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Install owned literals and remove both borrowed-history regions.
# Inputs: Complete admitted foundation source and a shipped wire version.
# Outputs: Private proposed text; no intermediate source is published or
# compiled.
function(superzip_zstd_canonical_boundaries content version output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals.cmake")
  string(TOUPPER "${version}" upper)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyLiterals${upper}.c"
       literals)
  string(REGEX REPLACE "\n+$" "" literals "${literals}")
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedLiterals.c" raw)
  string(REPLACE vXX "${version}" raw "${raw}")
  string(REGEX REPLACE "\n+$" "" raw "${raw}")
  string(REPLACE "/* Purpose: Decode rle literals"
                 "${raw}\n\n/* Purpose: Decode rle literals" literals
                 "${literals}")
  set(key "_zstd_legacy_literals_${version}")
  superzip_zstd_replace_owned_region("${content}" "${${key}_begin}"
                                     "${${key}_end}" "${literals}" content)
  superzip_zstd_rewrite_legacy_dictionary("${content}" "${version}" content)
  superzip_zstd_replace_owned_region(
    "${content}" "static void ZSTD${version}_checkContinuity("
    "static size_t ZSTD${version}_decompressBlock_internal(" "" content)
  if(version STREQUAL "v07")
    set(dictionary_return size_t)
  else()
    set(dictionary_return void)
  endif()
  superzip_zstd_replace_owned_region(
    "${content}" "static ${dictionary_return} ZSTD${version}_refDictContent("
    "static size_t ZSTD${version}_loadEntropy(" "" content)
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Normalize original history fields for the shared ownership rewrite.
# Inputs: Private source with canonical independent components installed.
# Outputs: Owned decoder geometry and complete entry guards before pointer
# formation.
function(superzip_zstd_canonical_geometry content version output)
  if(NOT version STREQUAL "v05")
    # Header transfers use the actual owned array extent. The private duplicate
    # maximum constant has no remaining reader; public upstream headers retain
    # their original interface declarations.
    string(CONCAT obsolete_header_max "static const size_t ZSTD${version}_"
                  "frameHeaderSize_max = ZSTD${version}_FRAMEHEADERSIZE_MAX;\n")
    string(REPLACE "${obsolete_header_max}" "" content "${content}")
  endif()
  string(REPLACE "    const void* vBase;" "    size_t dictSize;" content
                 "${content}")
  string(REPLACE "    dctx->vBase = NULL;" "    dctx->dictSize = 0;" content
                 "${content}")
  string(REPLACE "    const BYTE* const vBase = (const BYTE*) (dctx->vBase);"
                 "    size_t const dictSize = dctx->dictSize;" content
                 "${content}")
  string(REPLACE "base, vBase, dictEnd" "base, dictSize, dictEnd" content
                 "${content}")
  superzip_zstd_owned_decoder_geometry("${content}" "${version}" content)
  superzip_zstd_owned_decoder_guards("${content}" "${version}" content)
  string(REPLACE "    dctx->previousDstEnd = op;\n" "" content "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Install the complete stream ownership boundary from its original
# region. Inputs: Private decoder source and a shipped version. Outputs: Checked
# stream stages and matching context ownership, with no raw growth path.
function(superzip_zstd_canonical_stream content version output)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyStream.cmake")
  string(TOUPPER "${version}" upper)
  file(READ
       "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedStream${upper}.c"
       stream)
  string(REGEX REPLACE "\n+$" "" stream "${stream}")
  set(key "_zstd_legacy_stream_${version}")
  superzip_zstd_replace_owned_region("${content}" "${${key}_begin}"
                                     "${${key}_end}" "${stream}" content)
  string(
    REGEX
    REPLACE
      "(static|MEM_STATIC) size_t ZBUFF${version}_limitCopy\\([^}]*}[^}]*}" ""
      content "${content}")
  superzip_zstd_rewrite_owned_context("${content}" "${version}" content)
  if(version STREQUAL "v07")
    string(REPLACE "${_zstd_geometry_window_original}"
                   "${_zstd_geometry_window_checked}" content "${content}")
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Generate one complete canonical decoder entirely in private storage.
# Inputs: Exact immutable source text and one shipped version. Outputs: The
# complete owned production decoder, including canonical test-only probes.
function(superzip_zstd_generate_canonical content version output)
  superzip_zstd_canonical_foundation("${content}" "${version}" content)
  superzip_zstd_canonical_boundaries("${content}" "${version}" content)
  superzip_zstd_owned_decoder_components("${content}" "${version}" content)
  superzip_zstd_canonical_geometry("${content}" "${version}" content)
  superzip_zstd_canonical_stream("${content}" "${version}" content)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedProbe.c" probe)
  string(REPLACE vXX "${version}" probe "${probe}")
  string(SUBSTRING "${version}" 1 2 number)
  string(REGEX REPLACE "^0" "" number "${number}")
  string(APPEND content
         "\n#define SUPERZIP_LEGACY_DECODER_VERSION ${number}\n${probe}\n"
         "#undef SUPERZIP_LEGACY_DECODER_VERSION\n")
  string(SHA256 actual "${content}")
  if(NOT actual STREQUAL "${_zstd_canonical_${version}_patched}")
    message(
      FATAL_ERROR
        "Zstandard canonical output identity mismatch: ${version}; ${actual}")
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish complete canonical decoders from immutable provenance,
# migrating exact old caches. Inputs: A dependency root whose three current
# decoder hashes are all recognized. Outputs: Atomic individually verified final
# sources; drift and interrupted writes reject before generation.
function(superzip_patch_zstd_canonical source_dir)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyOwnedDecoder.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyDictionary.cmake")
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdTableGeometry.cmake")
  set(required FALSE)
  foreach(version IN ITEMS v05 v06 v07)
    unset(input_${version})
    set(source "${source_dir}/lib/legacy/zstd_${version}.c")
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${_zstd_canonical_${version}_patched}")
      continue()
    endif()
    superzip_zstd_canonical_input("${version}" "${actual}" recognized)
    if(NOT recognized)
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    if(EXISTS "${source}.superzip-patch" OR EXISTS
                                            "${source}.superzip-patch.raw")
      message(FATAL_ERROR "Incomplete or concurrent Zstandard patch: ${source}")
    endif()
    set(required TRUE)
    set(input_${version} "${actual}")
  endforeach()
  if(NOT required)
    return()
  endif()
  superzip_zstd_prepare_canonical_provenance("${source_dir}" pristine scratch)
  foreach(version IN ITEMS v05 v06 v07)
    if(NOT DEFINED input_${version})
      continue()
    endif()
    file(READ "${pristine}/lib/legacy/zstd_${version}.c" content)
    superzip_zstd_generate_canonical("${content}" "${version}"
                                     proposed_${version})
  endforeach()
  foreach(version IN ITEMS v05 v06 v07)
    if(DEFINED input_${version})
      superzip_write_verified_zstd_patch(
        "${source_dir}/lib/legacy/zstd_${version}.c" "${input_${version}}"
        "${_zstd_canonical_${version}_patched}" "${proposed_${version}}")
    endif()
  endforeach()
  superzip_zstd_release_canonical_provenance("${source_dir}" "${scratch}")
endfunction()

# Purpose: Prepare exact upstream decoder bytes in a fresh private directory.
# Inputs: The admitted dependency root; output names receive pristine and
# scratch paths. Outputs: Only three immutable source files and the verified v05
# foundation, or an explicit provenance/extraction error.
function(superzip_zstd_prepare_canonical_provenance source_dir output
         scratch_output)
  string(CONCAT archive "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/"
                "upstream/zstd/v1.5.7/zstd-v1.5.7.zip")
  file(SHA256 "${archive}" archive_hash)
  if(NOT archive_hash STREQUAL _zstd_canonical_archive_hash)
    message(FATAL_ERROR "Zstandard canonical provenance identity mismatch")
  endif()
  string(
    RANDOM
    LENGTH 16
    ALPHABET 0123456789abcdef nonce)
  set(scratch "${source_dir}/.superzip-canonical-${nonce}")
  if(EXISTS "${scratch}")
    message(FATAL_ERROR "Refusing a preexisting canonical generation directory")
  endif()
  file(MAKE_DIRECTORY "${scratch}")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -E tar xf "${archive}" --
      "zstd-1.5.7/lib/legacy/zstd_v05.c" "zstd-1.5.7/lib/legacy/zstd_v06.c"
      "zstd-1.5.7/lib/legacy/zstd_v07.c"
    WORKING_DIRECTORY "${scratch}"
    RESULT_VARIABLE result
    TIMEOUT 20)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Canonical Zstandard provenance extraction failed")
  endif()
  set(pristine "${scratch}/zstd-1.5.7")
  superzip_patch_zstd_v05("${pristine}")
  set(${output}
      "${pristine}"
      PARENT_SCOPE)
  set(${scratch_output}
      "${scratch}"
      PARENT_SCOPE)
endfunction()

# Purpose: Release only the completed generator's private provenance directory.
# Inputs: The admitted dependency root and its fresh scratch path. Outputs:
# Removes private files after real-path containment checks; failed proposals
# stay available for diagnosis.
function(superzip_zstd_release_canonical_provenance source_dir scratch)
  get_filename_component(resolved_source "${source_dir}" REALPATH)
  get_filename_component(resolved_scratch "${scratch}" REALPATH)
  string(FIND "${resolved_scratch}" "${resolved_source}/.superzip-canonical-"
              owned_prefix)
  if(NOT owned_prefix EQUAL 0 OR NOT resolved_scratch MATCHES
                                 "\\.superzip-canonical-[0-9a-f]+$")
    message(
      FATAL_ERROR "Refusing canonical cleanup outside private source storage")
  endif()
  file(REMOVE_RECURSE "${resolved_scratch}")
endfunction()
