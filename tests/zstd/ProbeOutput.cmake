include_guard(GLOBAL)

# Make direct cmake --build rerun generation when an actual input changes.
# Script-mode fixtures have no build graph; ordinary configurations retain all
# primary sources, shared definitions and owned bridges as configure inputs.
function(flitzzip_register_probe_inputs)
  if(NOT CMAKE_SCRIPT_MODE_FILE)
    foreach(input IN LISTS ARGN)
      get_filename_component(absolute_input "${input}" ABSOLUTE)
      set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${absolute_input}")
    endforeach()
  endif()
endfunction()

# Preserve compiler-input metadata when validated generated text is unchanged.
# This is a text boundary, not binary-file admission; callers validate inputs
# first.
function(flitzzip_write_probe_if_changed output content)
  if(EXISTS "${output}")
    file(READ "${output}" existing)
    if("${existing}" STREQUAL "${content}")
      return()
    endif()
  endif()
  file(WRITE "${output}" "${content}")
endfunction()
