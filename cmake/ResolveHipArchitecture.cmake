# Purpose: Resolve configure-time targets through the compiler wrapper's policy.
# Inputs: output names the receiving variable; selection is the requested preset
# or target list. Outputs: Returns canonical targets or fails closed.
function(superzip_resolve_hip_architecture output selection)
  execute_process(
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/hip_architecture.ps1"
      -Architecture "${selection}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE targets
    ERROR_VARIABLE diagnostics
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 15)
  if(NOT result STREQUAL "0"
     OR NOT targets MATCHES "^gfx[0-9a-z]+(,gfx[0-9a-z]+)*$")
    message(FATAL_ERROR "HIP architecture resolution failed: ${diagnostics}")
  endif()
  set(${output}
      "${targets}"
      PARENT_SCOPE)
endfunction()
