# This is a standalone LLVM external project, never a product build dependency.
if(NOT LLVM_ENABLE_ZLIB)
  message(FATAL_ERROR "Native profiling readers require LLVM zlib support")
endif()
set(_profile_tools "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(_profile_toolchain_file "${_profile_tools}/hip-coverage-toolchain.json")
set(_profile_toolchain_parser "${_profile_tools}/hip_coverage_toolchain.py")
if(NOT Python3_EXECUTABLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${_profile_toolchain_parser}" --lock
          "${_profile_toolchain_file}" --field revision
  TIMEOUT 15
  RESULT_VARIABLE _profile_lock_status
  OUTPUT_VARIABLE _profile_expected_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _profile_lock_status EQUAL 0)
  message(FATAL_ERROR "HIP profiling provenance admission failed")
endif()
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${_profile_toolchain_parser}" --lock
          "${_profile_toolchain_file}" --field llvm_major
  TIMEOUT 15
  RESULT_VARIABLE _profile_major_status
  OUTPUT_VARIABLE _profile_expected_major
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _profile_major_status EQUAL 0 OR NOT LLVM_VERSION_MAJOR EQUAL
                                        _profile_expected_major)
  message(
    FATAL_ERROR
      "HIP profiling registration requires the reviewed LLVM 24 source ABI")
endif()
find_package(Git REQUIRED)
get_filename_component(_profile_llvm_checkout "${LLVM_MAIN_SRC_DIR}/.."
                       ABSOLUTE)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${_profile_llvm_checkout}" rev-parse HEAD
  RESULT_VARIABLE _profile_revision_status
  OUTPUT_VARIABLE _profile_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _profile_revision_status EQUAL 0 OR NOT _profile_revision STREQUAL
                                           _profile_expected_revision)
  message(
    FATAL_ERROR
      "HIP profiling needs the exact reviewed ROCm LLVM source revision")
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${_profile_llvm_checkout}" diff --quiet HEAD
          -- llvm compiler-rt third-party
  RESULT_VARIABLE _profile_source_status)
if(NOT _profile_source_status EQUAL 0)
  message(
    FATAL_ERROR
      "The selected LLVM and profiling runtime source must be unmodified")
endif()

set(LLVM_LINK_COMPONENTS
    Core
    IRReader
    BitWriter
    Object
    ProfileData
    Coverage
    Support
    TransformUtils)
# The lifecycle bridge is compiled by the product's development DLL target.
set(LLVM_OPTIONAL_SOURCES hip_profile_lifecycle.cpp)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS
           "${CMAKE_CURRENT_SOURCE_DIR}/hip_profile_registration.cpp"
           "${_profile_toolchain_file}" "${_profile_toolchain_parser}")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/hip_profile_registration.cpp"
     _profile_tool_source_sha256)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _profile_tool_cmake_sha256)
file(SHA256 "${_profile_toolchain_file}" _profile_toolchain_sha256)
file(SHA256 "${_profile_toolchain_parser}" _profile_parser_sha)
add_llvm_tool(flitzzip_hip_profile_register hip_profile_registration.cpp
              DEPENDS intrinsics_gen)
target_compile_features(flitzzip_hip_profile_register PRIVATE cxx_std_20)
set_target_properties(flitzzip_hip_profile_register
                      PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
target_compile_definitions(
  flitzzip_hip_profile_register
  PRIVATE FLITZZIP_COVERAGE_LLVM_REVISION="${_profile_revision}"
          FLITZZIP_COVERAGE_TOOL_SOURCE_SHA256="${_profile_tool_source_sha256}"
          FLITZZIP_COVERAGE_TOOL_CMAKE_SHA256="${_profile_tool_cmake_sha256}"
          FLITZZIP_COVERAGE_TOOLCHAIN_SHA256="${_profile_toolchain_sha256}"
          FLITZZIP_COVERAGE_TOOLCHAIN_PARSER_SHA256="${_profile_parser_sha}")
target_include_directories(
  flitzzip_hip_profile_register SYSTEM
  PRIVATE "${LLVM_MAIN_SRC_DIR}/include" "${LLVM_BINARY_DIR}/include"
          "${LLVM_MAIN_SRC_DIR}/../compiler-rt/lib/profile"
          "${LLVM_MAIN_SRC_DIR}/../compiler-rt/include")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(flitzzip_hip_profile_register PRIVATE -Werror)
endif()
