# Configure and build a tiny owned project using the production declaration
# generator. This is a native correctness contract, not an offline parser test;
# it never builds the product or launches the fixture executables.
if(NOT DEFINED REPO_ROOT
   OR NOT DEFINED OUTPUT_ROOT
   OR NOT DEFINED TEST_GENERATOR)
  message(FATAL_ERROR "Repository, output root and generator are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
get_filename_component(output_root "${OUTPUT_ROOT}" REALPATH)
string(
  RANDOM
  LENGTH 16
  ALPHABET 0123456789abcdef identity)
set(_owned "${output_root}/native inventory space, λ-${identity}")
if(EXISTS "${_owned}" OR IS_SYMLINK "${_owned}")
  message(FATAL_ERROR "Fixture destination already exists")
endif()
set(_source "${_owned}/source")
set(_build "${_owned}/build")
file(MAKE_DIRECTORY "${_source}/child" "${_source}/hidden")
file(WRITE "${_source}/main.cpp" "int main() { return 0; }\n")
file(
  WRITE "${_source}/CMakeLists.txt"
  [=[
cmake_minimum_required(VERSION 3.24)
project(NativeInventoryContract LANGUAGES CXX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
add_executable(main_contract main.cpp)
add_executable(optional_contract EXCLUDE_FROM_ALL main.cpp)
set_property(TARGET optional_contract PROPERTY EXCLUDE_FROM_ALL "$<BOOL:1>")
add_library(interface_contract INTERFACE)
add_subdirectory(child)
add_subdirectory(hidden EXCLUDE_FROM_ALL)
include("${INVENTORY_MODULE}")
flitzzip_write_native_executable_inventory()
]=])
file(
  WRITE "${_source}/child/CMakeLists.txt"
  [=[
add_executable(later_contract ../main.cpp)
set_target_properties(later_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/nested/$<CONFIG>")
]=])
file(
  WRITE "${_source}/hidden/CMakeLists.txt"
  [=[
add_executable(hidden_contract ../main.cpp)
add_executable(explicit_contract ../main.cpp)
set_property(TARGET explicit_contract PROPERTY EXCLUDE_FROM_ALL FALSE)
]=])
set(_configure_args
    -S
    "${_source}"
    -B
    "${_build}"
    -G
    "${TEST_GENERATOR}"
    "-DINVENTORY_MODULE=${REPO_ROOT}/cmake/NativeExecutableInventory.cmake")
if(DEFINED TEST_PLATFORM AND NOT TEST_PLATFORM STREQUAL "")
  list(APPEND _configure_args -A "${TEST_PLATFORM}")
endif()
if(DEFINED TEST_TOOLSET AND NOT TEST_TOOLSET STREQUAL "")
  list(APPEND _configure_args -T "${TEST_TOOLSET}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" ${_configure_args}
  RESULT_VARIABLE status
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 120)
if(NOT status STREQUAL "0")
  message(
    FATAL_ERROR "Fixture configuration failed: ${status}\n${output}\n${error}")
endif()
foreach(configuration IN ITEMS Debug Release)
  file(READ "${_build}/${configuration}/flitzzip-native-executables.json"
       manifest)
  string(JSON actual_configuration GET "${manifest}" configuration)
  string(JSON count LENGTH "${manifest}" executables)
  if(NOT actual_configuration STREQUAL configuration OR NOT count EQUAL 5)
    message(FATAL_ERROR "Wrong executable declaration configuration or count")
  endif()
  set(_eligible)
  foreach(index RANGE 0 4 1)
    string(
      JSON
      target
      GET
      "${manifest}"
      executables
      ${index}
      target)
    string(
      JSON
      path
      GET
      "${manifest}"
      executables
      ${index}
      path)
    string(
      JSON
      included
      GET
      "${manifest}"
      executables
      ${index}
      default_build)
    if(included)
      list(APPEND _eligible "${target}")
    endif()
    if(target STREQUAL "later_contract")
      if(NOT path STREQUAL "nested/${configuration}/later_contract.exe")
        message(FATAL_ERROR "Nested output declaration is incorrect")
      endif()
    elseif(NOT path STREQUAL "${configuration}/${target}.exe")
      message(FATAL_ERROR "Executable path is not relative to the build")
    endif()
  endforeach()
  list(SORT _eligible)
  if(NOT _eligible STREQUAL "explicit_contract;later_contract;main_contract")
    message(FATAL_ERROR "Explicit or inherited target exclusions are incorrect")
  endif()
endforeach()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config Release --parallel 2
  RESULT_VARIABLE status
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 120)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "Fixture build failed: ${status}\n${output}\n${error}")
endif()
foreach(index RANGE 0 4 1)
  string(
    JSON
    path
    GET
    "${manifest}"
    executables
    ${index}
    path)
  string(
    JSON
    included
    GET
    "${manifest}"
    executables
    ${index}
    default_build)
  if((included AND NOT EXISTS "${_build}/${path}")
     OR (NOT included AND EXISTS "${_build}/${path}"))
    message(
      FATAL_ERROR "Default build output disagrees with executable declaration")
  endif()
endforeach()
# Remove only the newly created owned tree after resolving it again. Failed
# fixtures remain available for diagnosis; no existing build is removed.
get_filename_component(resolved "${_owned}" REALPATH)
cmake_path(IS_PREFIX output_root "${resolved}" NORMALIZE inside)
if(NOT inside
   OR NOT resolved STREQUAL _owned
   OR IS_SYMLINK "${_owned}")
  message(FATAL_ERROR "Fixture ownership changed before cleanup")
endif()
file(REMOVE_RECURSE "${_owned}")
message(STATUS "Executable declarations match the actual tiny default build")
