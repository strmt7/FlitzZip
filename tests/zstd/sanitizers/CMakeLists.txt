cmake_minimum_required(VERSION 4.4)
project(FlitzZipZstdSanitizerValidation LANGUAGES C CXX)

if(NOT WIN32
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
   OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
   OR NOT DEFINED REPO_ROOT)
  message(
    FATAL_ERROR
      "Zstandard sanitizer validation requires Windows x64 MSVC and REPO_ROOT")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(FLITZZIP_SOURCE_ROOT "${REPO_ROOT}")
set(_archive "${REPO_ROOT}/third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip")
file(SHA256 "${_archive}" archive_hash)
if(NOT archive_hash STREQUAL
   "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4")
  message(FATAL_ERROR "Sanitizer dependency archive identity mismatch")
endif()
set(_source "${CMAKE_CURRENT_BINARY_DIR}/source/zstd-1.5.7")
if(NOT EXISTS "${_source}/lib/zstd.h")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/source")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${_archive}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/source"
    RESULT_VARIABLE extraction_result)
  if(NOT extraction_result EQUAL 0)
    message(FATAL_ERROR "Sanitizer dependency extraction failed")
  endif()
endif()
include("${REPO_ROOT}/cmake/PatchZstdLegacy.cmake")
flitzzip_patch_zstd_legacy("${_source}")
include("${REPO_ROOT}/cmake/ZstdLibrary.cmake")
flitzzip_add_zstd_library("${_source}")
set(FLITZZIP_ZSTD_RUNTIME_DLL "$<TARGET_FILE:libzstd_shared>")

# Purpose: Place the exact instrumented validation DLL beside its test consumer.
# Inputs: target_name is a canonical test target. Outputs: Copies only the
# validation runtime after linking.
function(flitzzip_copy_zstd_runtime target_name)
  add_custom_command(
    TARGET "${target_name}"
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${FLITZZIP_ZSTD_RUNTIME_DLL}"
      "$<TARGET_FILE_DIR:${target_name}>/libzstd.dll"
    COMMENT "Copying canonical sanitizer validation runtime"
    VERBATIM)
endfunction()

enable_testing()
include("${REPO_ROOT}/cmake/ZstdTests.cmake")
add_executable(flitzzip_zstd_asan_control asan_control.cpp)
foreach(
  target IN
  ITEMS libzstd_shared
        flitzzip_zstd_cover_probe
        flitzzip_zstd_cover_owner_probe
        flitzzip_zstd_legacy_fault_objects
        flitzzip_zstd_legacy_buffer_probe
        flitzzip_zstd_bounds_tests
        flitzzip_zstd_cover_selection_tests
        flitzzip_zstd_legacy_tests
        flitzzip_zstd_asan_control)
  target_compile_options(
    "${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:/Zi>")
  get_target_property(type "${target}" TYPE)
  if(NOT type STREQUAL "OBJECT_LIBRARY")
    target_link_options("${target}" PRIVATE /INCREMENTAL:NO /DEBUG)
  endif()
endforeach()
get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_asan_runtime "${compiler_directory}/clang_rt.asan_dynamic-x86_64.dll")
if(NOT EXISTS "${_asan_runtime}")
  message(
    FATAL_ERROR "Installed MSVC x64 AddressSanitizer runtime is unavailable")
endif()
add_custom_command(
  TARGET flitzzip_zstd_asan_control
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_asan_runtime}"
          "$<TARGET_FILE_DIR:flitzzip_zstd_asan_control>"
  COMMENT "Copying installed MSVC sanitizer runtime"
  VERBATIM)
add_test(
  NAME flitzzip_zstd_asan_qualification
  COMMAND
    "${CMAKE_COMMAND}" "-DCONTROL=$<TARGET_FILE:flitzzip_zstd_asan_control>" -P
    "${REPO_ROOT}/tests/cmake/test_zstd_asan_control.cmake")
set_tests_properties(flitzzip_zstd_asan_qualification PROPERTIES TIMEOUT 45)
set_tests_properties(
  flitzzip_zstd_bounds_tests flitzzip_zstd_cover_selection_tests
  flitzzip_zstd_legacy_tests PROPERTIES DEPENDS
                                        flitzzip_zstd_asan_qualification)
# Retain the shared composite/child deadlines from ZstdTests.cmake.
# This standalone project has no product, HIP backend, installer or packaging
# targets.
