cmake_minimum_required(VERSION 3.24)

if(NOT IS_ABSOLUTE "${REPO_ROOT}" OR NOT IS_ABSOLUTE "${OUTPUT_ROOT}")
  message(FATAL_ERROR "HIP output contracts require explicit absolute roots")
endif()
cmake_path(CONVERT "${REPO_ROOT}" TO_CMAKE_PATH_LIST REPO_ROOT NORMALIZE)
cmake_path(CONVERT "${OUTPUT_ROOT}" TO_CMAKE_PATH_LIST OUTPUT_ROOT NORMALIZE)
if(NOT DEFINED HIP_OBJECT_MODULE)
  set(HIP_OBJECT_MODULE "${REPO_ROOT}/cmake/HipKernelObjects.cmake")
endif()
string(
  RANDOM
  LENGTH 16
  ALPHABET 0123456789abcdef run)
set(_run_root "${OUTPUT_ROOT}/${run}")
foreach(case IN ITEMS undefined empty relative valid)
  set(_work "${_run_root}/${case}")
  if(EXISTS "${_work}")
    message(FATAL_ERROR "Preserve previous HIP output contract evidence")
  endif()
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${REPO_ROOT}/tests/cmake/hip_object_output" -B
      "${_work}" "-DOUTPUT_CASE=${case}"
      "-DHIP_OBJECT_MODULE=${HIP_OBJECT_MODULE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 60)
  if(case STREQUAL "valid")
    if(NOT result STREQUAL "0" OR NOT EXISTS "${_work}/admitted.txt")
      message(FATAL_ERROR "Valid HIP output was rejected: ${output}${error}")
    endif()
    file(READ "${_work}/admitted.txt" actual)
    if(NOT actual STREQUAL "${_work}/owned.obj")
      message(FATAL_ERROR "HIP output identity changed")
    endif()
  elseif(
    result STREQUAL "0"
    OR EXISTS "${_work}/admitted.txt"
    OR NOT "${output}${error}" MATCHES
       "HIP object requires an absolute nonempty output")
    message(
      FATAL_ERROR
        "Invalid HIP output was not rejected before command generation: "
        "${case}: ${output}${error}")
  endif()
endforeach()
message(STATUS "HIP object output admission contracts passed")
