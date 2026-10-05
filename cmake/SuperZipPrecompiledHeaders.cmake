# Purpose: Avoid repeated standard-library parsing in the three largest MSVC C++
# targets. Inputs: Existing optional GUI/test and core targets; their own
# compiler flags and definitions. Outputs: Private, per-target C++ precompiled
# headers; leaves C, HIP, project and vendor headers untouched.
function(superzip_enable_precompiled_headers)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    return()
  endif()
  foreach(target IN ITEMS superzip_core superzip_test_objects SuperZip)
    if(TARGET "${target}")
      foreach(
        header IN
        ITEMS algorithm
              array
              atomic
              chrono
              condition_variable
              cstdint
              deque
              filesystem
              functional
              memory
              mutex
              optional
              span
              string
              string_view
              thread
              unordered_map
              vector)
        target_precompile_headers(
          "${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:<${header}$<ANGLE-R>>")
      endforeach()
    endif()
  endforeach()
endfunction()
