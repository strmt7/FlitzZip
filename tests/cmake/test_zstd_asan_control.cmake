# Purpose: Require working instrumentation rather than accepting an ASan
# compiler flag as runtime evidence. Inputs: CONTROL names the isolated
# instrumented fixture. Outputs: Fails unless valid access succeeds and invalid
# access fails specifically with an AddressSanitizer heap-buffer-overflow
# diagnostic.
if(NOT DEFINED CONTROL OR NOT EXISTS "${CONTROL}")
  message(FATAL_ERROR "Sanitizer qualification executable is missing")
endif()
execute_process(
  COMMAND "${CONTROL}" --valid
  RESULT_VARIABLE positive
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 15)
string(STRIP "${output}" output)
if(NOT positive EQUAL 0 OR NOT output STREQUAL "92")
  message(FATAL_ERROR "Sanitizer positive control failed: ${positive} ${error}")
endif()
execute_process(
  COMMAND "${CONTROL}" --heap-overflow
  RESULT_VARIABLE negative
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 15)
if(NOT negative MATCHES "^[1-9][0-9]*$"
   OR NOT "${output}${error}" MATCHES "AddressSanitizer: heap-buffer-overflow")
  message(
    FATAL_ERROR
      "Sanitizer negative control was not detected: ${negative} ${error}")
endif()
message(STATUS "MSVC ASan valid access and heap-overflow rejection qualified")
