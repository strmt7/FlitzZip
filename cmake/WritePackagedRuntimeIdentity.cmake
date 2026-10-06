# Purpose: Bind packaged runtime metadata to actual linked bytes. Inputs: Built
# Zstandard DLL, pinned wimlib DLL, base manifest and output; HIP additionally
# requires its built module, digest header and product version. Outputs: Stable
# final manifest and optional kernel header; rejects missing or altered inputs.
foreach(required IN ITEMS ZSTD_DLL WIM_DLL BASE_MANIFEST MANIFEST)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing packaged runtime identity input: ${required}")
  endif()
endforeach()
file(READ "${BASE_MANIFEST}" _manifest LIMIT 65536)
string(JSON _count LENGTH "${_manifest}" packaged_runtime_files)
string(JSON _hip_enabled GET "${_manifest}" gpu_backend enabled)
if(NOT _count EQUAL 2)
  message(FATAL_ERROR "Base manifest must contain the two compatibility DLLs")
endif()
foreach(index RANGE 0 1 1)
  if(index EQUAL 0)
    set(_runtime "${ZSTD_DLL}")
    set(_expected_name "libzstd.dll")
  else()
    set(_runtime "${WIM_DLL}")
    set(_expected_name "libwim-15.dll")
  endif()
  if(NOT EXISTS "${_runtime}" OR IS_DIRECTORY "${_runtime}")
    message(FATAL_ERROR "Packaged runtime DLL is missing: ${_runtime}")
  endif()
  get_filename_component(_name "${_runtime}" NAME)
  string(
    JSON
    _manifest_name
    GET
    "${_manifest}"
    packaged_runtime_files
    ${index}
    name)
  if(NOT _name STREQUAL _expected_name OR NOT _manifest_name STREQUAL
                                          _expected_name)
    message(FATAL_ERROR "Unexpected packaged runtime filename: ${_name}")
  endif()
  file(SHA256 "${_runtime}" _digest)
  if(index EQUAL 1)
    string(JSON _pinned_digest GET "${_manifest}" packaged_runtime_files 1
           sha256)
    if(NOT _digest STREQUAL _pinned_digest)
      message(FATAL_ERROR "wimlib bytes differ from pinned runtime identity")
    endif()
  endif()
  string(JSON _manifest SET "${_manifest}" packaged_runtime_files ${index}
         sha256 "\"${_digest}\"")
endforeach()
if(_hip_enabled)
  foreach(required IN ITEMS INPUT HEADER PACKAGE_VERSION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
      message(FATAL_ERROR "Missing HIP identity input: ${required}")
    endif()
  endforeach()
  if(NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
    message(FATAL_ERROR "Built HIP kernel DLL is missing: ${INPUT}")
  endif()
  get_filename_component(_name "${INPUT}" NAME)
  if(NOT _name STREQUAL "superzip_hip_kernels.dll")
    message(FATAL_ERROR "Unexpected kernel module filename: ${_name}")
  endif()
  file(SHA256 "${INPUT}" _digest)
  string(JSON _architectures GET "${_manifest}" gpu_backend arch)
  if(NOT _architectures MATCHES "^gfx[0-9a-z]+(,gfx[0-9a-z]+)*$")
    message(FATAL_ERROR "Kernel identity requires canonical compiled targets")
  endif()
  string(REPLACE "," ";" _targets "${_architectures}")
  list(LENGTH _targets _target_count)
  set(_unique_targets "${_targets}")
  list(REMOVE_DUPLICATES _unique_targets)
  list(LENGTH _unique_targets _unique_count)
  if(_target_count GREATER 16 OR NOT _target_count EQUAL _unique_count)
    message(FATAL_ERROR "Kernel identity requires bounded unique targets")
  endif()
  set(_target_literals "")
  foreach(target IN LISTS _targets)
    string(APPEND _target_literals "\"${target}\",")
  endforeach()
  string(
    CONCAT _header_text
           "#pragma once\n\n#include <array>\n"
           "#include <string_view>\n\n"
           "inline constexpr char kHipKernelModuleSha256[] = "
           "\"${_digest}\";\n"
           "inline constexpr std::array<std::string_view, "
           "${_target_count}> kHipKernelArchitectures{"
           "${_target_literals}};\n")
  get_filename_component(_header_directory "${HEADER}" DIRECTORY)
  file(MAKE_DIRECTORY "${_header_directory}")
  file(WRITE "${HEADER}.pending" "${_header_text}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HEADER}.pending"
            "${HEADER}" COMMAND_ERROR_IS_FATAL ANY)
  file(REMOVE "${HEADER}.pending")
  set(_entry "{}")
  string(JSON _entry SET "${_entry}" name "\"${_name}\"")
  string(JSON _entry SET "${_entry}" version "\"${PACKAGE_VERSION}\"")
  string(JSON _entry SET "${_entry}" sha256 "\"${_digest}\"")
  string(JSON _entry SET "${_entry}" source_kind "\"first-party-native\"")
  string(
    JSON _entry SET "${_entry}" reason
    "\"Kernel module admitted after trusted device and exact-byte checks.\"")
  string(
    JSON
    _manifest
    SET
    "${_manifest}"
    packaged_runtime_files
    ${_count}
    "${_entry}")
elseif(DEFINED INPUT OR DEFINED HEADER)
  message(FATAL_ERROR "CPU validation manifest must not admit a HIP module")
endif()
file(WRITE "${MANIFEST}.pending" "${_manifest}\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MANIFEST}.pending"
          "${MANIFEST}" COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${MANIFEST}.pending")
