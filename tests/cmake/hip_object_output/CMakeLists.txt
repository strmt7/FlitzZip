cmake_minimum_required(VERSION 3.24)
project(HipObjectOutputContract LANGUAGES NONE)

include("${HIP_OBJECT_MODULE}")
if(OUTPUT_CASE STREQUAL "empty")
  set(FIXTURE_OUTPUT "")
elseif(OUTPUT_CASE STREQUAL "relative")
  set(FIXTURE_OUTPUT "unowned.obj")
elseif(OUTPUT_CASE STREQUAL "valid")
  set(FIXTURE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/owned.obj")
elseif(NOT OUTPUT_CASE STREQUAL "undefined")
  message(FATAL_ERROR "Unknown HIP output test case")
endif()

# Configure only: no HIP compiler, source execution or native build is needed.
flitzzip_compile_hip_object(CMakeLists.txt FIXTURE_OUTPUT
                            "Output admission control")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/admitted.txt" "${FIXTURE_OUTPUT}")
