cmake_minimum_required(VERSION 4.4)

set(ASAN_CONTROL flitzzip_zstd_asan_qualification)
set(ASAN_FIXTURE flitzzip_zstd_asan_runtime)
set(ASAN_CONSUMERS
    flitzzip_zstd_bounds_tests flitzzip_zstd_cover_selection_tests
    flitzzip_zstd_legacy_tests)

# Require the exact production prerequisite, rather than inventing a fixture
# inside the harmless probe and testing only that invented configuration.
function(flitzzip_assert_asan_fixture name properties)
  if(name STREQUAL ASAN_CONTROL)
    set(required FIXTURES_SETUP)
  else()
    set(required FIXTURES_REQUIRED)
  endif()
  string(JSON count LENGTH "${properties}")
  math(EXPR last "${count} - 1")
  set(found FALSE)
  foreach(index RANGE 0 ${last} 1)
    string(JSON key GET "${properties}" ${index} name)
    if(key STREQUAL required)
      string(JSON values GET "${properties}" ${index} value)
      string(JSON value_count LENGTH "${values}")
      string(JSON value GET "${values}" 0)
      if(NOT value_count EQUAL 1 OR NOT value STREQUAL ASAN_FIXTURE)
        message(FATAL_ERROR "Unexpected sanitizer fixture on ${name}")
      endif()
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "Sanitizer test ${name} is missing ${required}")
  endif()
endfunction()

# Admit every configured native consumer and its setup control exactly once.
function(flitzzip_assert_asan_inventory inventory)
  string(JSON count LENGTH "${inventory}" tests)
  math(EXPR last "${count} - 1")
  set(admitted)
  foreach(index RANGE 0 ${last} 1)
    string(JSON test GET "${inventory}" tests ${index})
    string(JSON name GET "${test}" name)
    if(NOT name STREQUAL ASAN_CONTROL AND NOT name IN_LIST ASAN_CONSUMERS)
      continue()
    endif()
    if(name IN_LIST admitted)
      message(FATAL_ERROR "Duplicate sanitizer test: ${name}")
    endif()
    string(JSON properties GET "${test}" properties)
    flitzzip_assert_asan_fixture("${name}" "${properties}")
    list(APPEND admitted "${name}")
  endforeach()
  list(LENGTH admitted admitted_count)
  if(NOT admitted_count EQUAL 4)
    message(FATAL_ERROR "The complete sanitizer cohort was not configured")
  endif()
endfunction()

# METADATA is CTest's actual inventory; CTEST and WORK_ROOT select the pinned
# executor and owned scratch parent. Retain the failed setup run, and reject any
# dependent execution without ever launching a sanitizer executable.
function(flitzzip_test_asan_gating)
  foreach(required IN ITEMS METADATA CTEST WORK_ROOT)
    if(NOT DEFINED ${required})
      message(FATAL_ERROR "Missing sanitizer gating input: ${required}")
    endif()
  endforeach()
  file(TO_CMAKE_PATH "${WORK_ROOT}" work_root)
  file(READ "${METADATA}" inventory)
  flitzzip_assert_asan_inventory("${inventory}")
  string(
    RANDOM
    LENGTH 20
    ALPHABET 0123456789abcdef run_id)
  set(scratch "${work_root}/asan-gating-${run_id}")
  if(EXISTS "${scratch}")
    message(FATAL_ERROR "Refusing an existing sanitizer gating directory")
  endif()
  file(MAKE_DIRECTORY "${scratch}")
  set(manifest "${scratch}/CTestTestfile.cmake")
  file(
    WRITE "${manifest}"
    "add_test(${ASAN_CONTROL} \"${CMAKE_COMMAND}\" -E false)\n"
    "set_tests_properties(${ASAN_CONTROL} PROPERTIES "
    "FIXTURES_SETUP ${ASAN_FIXTURE})\n")
  foreach(name IN LISTS ASAN_CONSUMERS)
    file(
      APPEND "${manifest}"
      "add_test(${name} \"${CMAKE_COMMAND}\" "
      "-E touch \"${scratch}/${name}.started\")\n"
      "set_tests_properties(${name} PROPERTIES "
      "FIXTURES_REQUIRED ${ASAN_FIXTURE})\n")
  endforeach()
  execute_process(
    COMMAND "${CTEST}" --test-dir "${scratch}" --interactive-debug-mode 0
            --output-on-failure --output-junit "${scratch}/result.xml"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 30)
  file(WRITE "${scratch}/output.txt" "${output}${error}")
  if(NOT result STREQUAL "8")
    message(FATAL_ERROR "Failed fixture did not fail CTest: ${result}")
  endif()
  foreach(name IN LISTS ASAN_CONSUMERS)
    if(EXISTS "${scratch}/${name}.started")
      message(FATAL_ERROR "Failed qualification allowed ${name} to execute")
    endif()
    if(NOT output MATCHES "${name}[^\n]*Not Run")
      message(FATAL_ERROR "CTest did not report ${name} as blocked")
    endif()
  endforeach()
  message(STATUS "All sanitizer consumers were blocked by the failed fixture")
endfunction()

flitzzip_test_asan_gating()
