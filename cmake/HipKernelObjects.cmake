# Purpose: Prepare one HIP translation unit with the pinned Windows HIP path.
# Inputs: source is repo-relative, output_variable selects its object path, and
# comment is the build status text. Outputs: Produces an object and a compiler
# dependency file, tracking the headers consumed by this translation unit.
function(flitzzip_compile_hip_object source output_variable comment)
  if(NOT DEFINED ${output_variable}
     OR "${${output_variable}}" STREQUAL ""
     OR NOT IS_ABSOLUTE "${${output_variable}}")
    message(
      FATAL_ERROR
        "HIP object requires an absolute nonempty output: ${output_variable}")
  endif()
  if(FLITZZIP_HIP_COVERAGE_MANIFEST)
    string(JSON _coverage_object GET "${_coverage_metadata}" objects
           "${source}")
    set(${output_variable}
        "${_coverage_object}"
        PARENT_SCOPE)
    return()
  endif()
  set(output "${${output_variable}}")
  add_custom_command(
    OUTPUT "${output}"
    DEPFILE "${output}.d"
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1" -Source
      "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -Output "${output}" -RepoRoot
      "${CMAKE_CURRENT_SOURCE_DIR}" -Arch "${FLITZZIP_HIP_ARCH}" -HipPath
      "${FLITZZIP_HIP_PATH}" -VcvarsVersion "${FLITZZIP_VCVARS_VERSION}"
      -DependencyFile "${output}.d"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/hip_architecture.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/rocm_toolchain.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/process_environment.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/rocm-sdk-lock.json"
    COMMENT "${comment}"
    VERBATIM)
endfunction()
