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
set(SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256 "fixture-wim")
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
message(STATUS "CPU/HIP runtime manifest contracts passed")
