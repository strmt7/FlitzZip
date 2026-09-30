# Purpose: Bind runtime loading to the DLL built from pinned source.
# Inputs: RUNTIME_DLL is the built artifact; OUTPUT_HEADER is generated-only.
# Outputs: Writes its SHA-256 identity on change; rejects missing input.
if(NOT EXISTS "${RUNTIME_DLL}" OR NOT DEFINED OUTPUT_HEADER)
  message(FATAL_ERROR
          "Runtime identity requires a built DLL and output header")
endif()
file(SHA256 "${RUNTIME_DLL}" runtime_hash)
string(CONCAT HEADER_CONTENT "#pragma once\n"
              "#define SUPERZIP_ZSTD_RUNTIME_DLL_SHA256 \"${runtime_hash}\"\n")
if(EXISTS "${OUTPUT_HEADER}")
  file(READ "${OUTPUT_HEADER}" existing_content)
  if(existing_content STREQUAL HEADER_CONTENT)
    return()
  endif()
endif()
get_filename_component(output_directory "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT_HEADER}" "${HEADER_CONTENT}")
