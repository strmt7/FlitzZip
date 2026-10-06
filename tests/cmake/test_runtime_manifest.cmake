# Purpose: Exercise the production CPU/HIP manifest writer without an SDK.
# Inputs: REPO_ROOT and OUTPUT_ROOT locate templates and test output. Outputs:
# Requires common dependencies and truthful optional HIP scope.
if(NOT DEFINED REPO_ROOT OR NOT DEFINED OUTPUT_ROOT)
  message(FATAL_ERROR "REPO_ROOT and OUTPUT_ROOT are required")
endif()
include("${REPO_ROOT}/cmake/ConfigureRuntimeDependencies.cmake")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
set(SUPERZIP_PACKAGE_VERSION "0.8.0")
set(SUPERZIP_HIP_ARCH "gfx1201")
set(SUPERZIP_ZSTD_RUNTIME_DLL_SHA256 "fixture-zstd")
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE_SHA256 "fixture-zstd-package")
set(_zstd "${OUTPUT_ROOT}/libzstd.dll")
set(_wim "${OUTPUT_ROOT}/libwim-15.dll")
file(WRITE "${_wim}" "pinned-wim-fixture")
file(SHA256 "${_wim}" SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256)
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE_SHA256 "fixture-wim-package")
foreach(mode IN ITEMS OFF ON)
  if(mode STREQUAL "ON")
    set(SUPERZIP_HIP_RUNTIME_DLL_NAME "amdhip64_7.dll")
    set(SUPERZIP_HIP_RUNTIME_DLL_SHA256 "fixture-hip")
    set(SUPERZIP_HIP_RUNTIME_DLL_SIZE 12345)
  else()
    unset(SUPERZIP_HIP_RUNTIME_DLL_NAME)
    unset(SUPERZIP_HIP_RUNTIME_DLL_SHA256)
    unset(SUPERZIP_HIP_RUNTIME_DLL_SIZE)
  endif()
  set(OUTPUT_FILE "${OUTPUT_ROOT}/runtime-${mode}.json")
  superzip_configure_runtime_dependencies("${REPO_ROOT}/cmake" "${OUTPUT_FILE}"
                                          "${mode}")
  file(READ "${OUTPUT_FILE}" manifest LIMIT 65536)
  string(JSON enabled GET "${manifest}" gpu_backend enabled)
  string(JSON dependency_count LENGTH "${manifest}" packaged_runtime_files)
  string(JSON prerequisites_count LENGTH "${manifest}" host_prerequisites)
  string(
    JSON
    zstd_name
    GET
    "${manifest}"
    packaged_runtime_files
    0
    name)
  string(
    JSON
    wim_name
    GET
    "${manifest}"
    packaged_runtime_files
    1
    name)
  if(NOT dependency_count EQUAL 2
     OR NOT zstd_name STREQUAL "libzstd.dll"
     OR NOT wim_name STREQUAL "libwim-15.dll")
    message(FATAL_ERROR "CPU/HIP manifests must retain both app-local runtimes")
  endif()
  if(mode STREQUAL "OFF")
    if(enabled OR NOT prerequisites_count EQUAL 0)
      message(FATAL_ERROR "CPU validation manifest incorrectly requires HIP")
    endif()
  else()
    string(
      JSON
      runtime_name
      GET
      "${manifest}"
      host_prerequisites
      0
      required_runtime_dll)
    string(
      JSON
      runtime_size
      GET
      "${manifest}"
      host_prerequisites
      0
      build_sdk_runtime_size_bytes)
    if(NOT enabled
       OR NOT prerequisites_count EQUAL 1
       OR NOT runtime_name STREQUAL "amdhip64_7.dll"
       OR NOT runtime_size EQUAL 12345)
      message(FATAL_ERROR "HIP manifest lost its explicit AMD prerequisite")
    endif()
  endif()
endforeach()

# All runtime identities must follow linked bytes across incremental builds.
set(_module "${OUTPUT_ROOT}/superzip_hip_kernels.dll")
set(_base "${OUTPUT_ROOT}/runtime-ON.json")
set(_header "${OUTPUT_ROOT}/kernel-identity.hpp")
set(_final_manifest "${OUTPUT_ROOT}/kernel-manifest.json")
foreach(mode IN ITEMS OFF ON)
  set(_optional)
  if(mode STREQUAL "ON")
    list(APPEND _optional "-DINPUT=${_module}" "-DHEADER=${_header}"
         "-DPACKAGE_VERSION=${SUPERZIP_PACKAGE_VERSION}")
  endif()
  foreach(content IN ITEMS first-linked-bytes replacement-linked-bytes)
    file(WRITE "${_zstd}" "zstd-${content}")
    file(WRITE "${_module}" "kernel-${content}")
    file(SHA256 "${_zstd}" expected_zstd)
    file(SHA256 "${_module}" expected_kernel)
    execute_process(
      COMMAND
        "${CMAKE_COMMAND}" "-DZSTD_DLL=${_zstd}" "-DWIM_DLL=${_wim}"
        "-DBASE_MANIFEST=${OUTPUT_ROOT}/runtime-${mode}.json"
        "-DMANIFEST=${_final_manifest}" ${_optional} -P
        "${REPO_ROOT}/cmake/WritePackagedRuntimeIdentity.cmake"
      RESULT_VARIABLE status
      OUTPUT_VARIABLE log
      ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "Runtime identity producer failed: ${log}${error}")
    endif()
    file(READ "${_final_manifest}" manifest_text)
    string(JSON zstd_digest GET "${manifest_text}" packaged_runtime_files 0
           sha256)
    string(JSON wim_digest GET "${manifest_text}" packaged_runtime_files 1
           sha256)
    string(JSON module_count LENGTH "${manifest_text}" packaged_runtime_files)
    if(NOT zstd_digest STREQUAL expected_zstd
       OR NOT wim_digest STREQUAL SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256)
      message(
        FATAL_ERROR "Compatibility identities do not match actual DLL bytes")
    endif()
    if(mode STREQUAL "ON")
      file(READ "${_header}" header_text)
      string(JSON module_digest GET "${manifest_text}" packaged_runtime_files 2
             sha256)
      string(
        JSON
        module_name
        GET
        "${manifest_text}"
        packaged_runtime_files
        2
        name)
      if(NOT module_digest STREQUAL expected_kernel
         OR NOT module_count EQUAL 3
         OR NOT module_name STREQUAL "superzip_hip_kernels.dll"
         OR NOT header_text MATCHES "${expected_kernel}"
         OR NOT header_text MATCHES "kHipKernelArchitectures"
         OR NOT header_text MATCHES "gfx1201")
        message(
          FATAL_ERROR "Kernel identity does not match actual module bytes")
      endif()
    elseif(NOT module_count EQUAL 2)
      message(FATAL_ERROR "CPU validation manifest admitted an optional kernel")
    endif()
  endforeach()
endforeach()
foreach(
  failure IN
  ITEMS missing-kernel
        missing-zstd
        missing-wim
        changed-wim
        cpu-with-kernel
        invalid-target
        duplicate-target)
  set(_input "${_module}")
  set(_zstd_input "${_zstd}")
  set(_wim_input "${_wim}")
  set(_base "${OUTPUT_ROOT}/runtime-ON.json")
  if(failure STREQUAL "missing-kernel")
    set(_input "${OUTPUT_ROOT}/missing.dll")
  elseif(failure STREQUAL "missing-zstd")
    set(_zstd_input "${OUTPUT_ROOT}/missing.dll")
  elseif(failure STREQUAL "missing-wim")
    set(_wim_input "${OUTPUT_ROOT}/missing.dll")
  elseif(failure STREQUAL "changed-wim")
    file(WRITE "${_wim}" "altered-wim-fixture")
  elseif(failure STREQUAL "invalid-target" OR failure STREQUAL
                                              "duplicate-target")
    file(READ "${_base}" invalid_manifest)
    if(failure STREQUAL "invalid-target")
      set(_invalid_arch "gfx1201;unexpected")
    else()
      set(_invalid_arch "gfx1201,gfx1201")
    endif()
    string(
      JSON
      invalid_manifest
      SET
      "${invalid_manifest}"
      gpu_backend
      arch
      "\"${_invalid_arch}\"")
    set(_base "${OUTPUT_ROOT}/invalid-target.json")
    file(WRITE "${_base}" "${invalid_manifest}")
  else()
    set(_base "${OUTPUT_ROOT}/runtime-OFF.json")
  endif()
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" "-DZSTD_DLL=${_zstd_input}" "-DWIM_DLL=${_wim_input}"
      "-DINPUT=${_input}" "-DBASE_MANIFEST=${_base}" "-DHEADER=${_header}"
      "-DMANIFEST=${_final_manifest}"
      "-DPACKAGE_VERSION=${SUPERZIP_PACKAGE_VERSION}" -P
      "${REPO_ROOT}/cmake/WritePackagedRuntimeIdentity.cmake"
    RESULT_VARIABLE status
    OUTPUT_QUIET ERROR_QUIET)
  if(status EQUAL 0)
    message(FATAL_ERROR "Runtime identity producer admitted ${failure}")
  endif()
  file(WRITE "${_wim}" "pinned-wim-fixture")
endforeach()
message(STATUS "CPU/HIP runtime manifest contracts passed")
