cmake_minimum_required(VERSION 4.4)
project(SuperZipCompilerAbiContracts LANGUAGES C CXX)
if(NOT DEFINED ORIGINAL_HEADER OR NOT DEFINED REPAIRED_HEADER)
  message(FATAL_ERROR "Original and repaired ABI header paths are required")
endif()

# Purpose: Compile and run the actual ABI payload in C and C++. Inputs: Exact
# original/repaired header, extension and repetition flag. Outputs: Requires
# pointer-size/byte-order markers; original repeated inclusion must fail
# specifically on definition conflicts, while guarded inclusion works.
function(require_abi_probe header extension repeat expected_success)
  set(source "#include \"${header}\"\n")
  if(repeat)
    string(APPEND source "#include \"${header}\"\n")
  endif()
  string(
    APPEND
    source
    [=[
#include <string.h>
/* Purpose: Verify the unchanged compiler ABI marker payload.
 * Inputs: Native pointer geometry and the included probe arrays.
 * Outputs: Zero for exact pointer-size and native byte-order markers. */
int main(void) {
    char expected[] = "INFO:sizeof_dptr[00]";
    unsigned short const native = 1;
    int little = *(unsigned char const*)&native == 1;
    char const* marker = little ? (char const*)info_byte_order_little_endian
                               : (char const*)info_byte_order_big_endian;
    expected[17] = (char)('0' + ((sizeof(void*) / 10) % 10));
    expected[18] = (char)('0' + (sizeof(void*) % 10));
#if defined(ABI_ID)
    if (strcmp(info_abi, "INFO:abi[" ABI_ID "]") != 0) return 1;
#endif
#if defined(ARCHITECTURE_ID)
    if (strcmp(info_arch, "INFO:arch[" ARCHITECTURE_ID "]") != 0) return 1;
#endif
    return strcmp(info_sizeof_dptr, expected) != 0 ||
           strcmp(marker, little ? "INFO:byte_order[LITTLE_ENDIAN]"
                                 : "INFO:byte_order[BIG_ENDIAN]") != 0;
}
]=])
  try_run(
    ran
    compiled
    SOURCE_FROM_CONTENT
    "probe.${extension}"
    "${source}"
    NO_CACHE
    C_STANDARD
    11
    CXX_STANDARD
    20
    COMPILE_OUTPUT_VARIABLE diagnostic)
  if(expected_success)
    if(NOT compiled OR NOT ran EQUAL 0)
      message(FATAL_ERROR "Compiler ABI payload contract failed: ${diagnostic}")
    endif()
  elseif(compiled OR NOT diagnostic MATCHES "redefinition|C2086|conflicting")
    message(FATAL_ERROR "Compiler ABI negative control failed: ${diagnostic}")
  endif()
endfunction()

foreach(extension IN ITEMS c cpp)
  require_abi_probe("${ORIGINAL_HEADER}" "${extension}" FALSE TRUE)
  require_abi_probe("${ORIGINAL_HEADER}" "${extension}" TRUE FALSE)
  require_abi_probe("${REPAIRED_HEADER}" "${extension}" FALSE TRUE)
  require_abi_probe("${REPAIRED_HEADER}" "${extension}" TRUE TRUE)
endforeach()
