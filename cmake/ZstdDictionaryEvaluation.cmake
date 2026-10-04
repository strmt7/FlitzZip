# Purpose: Remove the superseded C evaluator after its C++ owner is linked.
# Inputs: Complete known predecessor text. Outputs: Preserves all adjacent
# helpers and declarations; rejects a missing implementation boundary.
function(superzip_rewrite_zstd_dictionary_evaluation content output)
  string(CONCAT marker "/* Purpose: Measure sample compression using "
                "the candidate dictionary.")
  string(FIND "${content}" "${marker}" begin)
  string(FIND "${content}" "/**\n * Initialize the `COVER_best_t`." end)
  if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Zstandard dictionary evaluation boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  string(CONCAT replacement "/* Dictionary evaluation is implemented by "
                "the checked C++ owner. */\n\n")
  string(REPLACE "${_zstd_evaluation_sum_original}"
                 "${_zstd_evaluation_sum_checked}" content
                 "${prefix}${replacement}${suffix}")
  string(REPLACE "#include <stdlib.h>"
                 "#include <stdint.h>\n#include <stdlib.h>" content
                 "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish exact checked sample evaluation and accumulation boundaries.
# Inputs: Extracted pinned COVER source after preceding repairs. Outputs:
# Applies once or rejects unknown drift; provenance and public APIs remain
# intact.
function(superzip_patch_zstd_dictionary_evaluation source_dir)
  set(source "${source_dir}/lib/dictBuilder/cover.c")
  file(SHA256 "${source}" actual)
  if(actual STREQUAL _zstd_dictionary_evaluation_patched)
    return()
  endif()
  superzip_zstd_patch_is_superseded(
    "${actual}" "${_zstd_dictionary_evaluation_patched}" superseded)
  if(superseded)
    return()
  endif()
  if(NOT actual STREQUAL _zstd_dictionary_evaluation_original)
    message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
  endif()
  file(READ "${source}" content)
  superzip_rewrite_zstd_dictionary_evaluation("${content}" content)
  superzip_write_verified_zstd_patch(
    "${source}" "${actual}" "${_zstd_dictionary_evaluation_patched}"
    "${content}")
endfunction()

set(_zstd_dictionary_evaluation_original
    "2c99b6e65bf7d999920d20206efe9dffdd86bc1c3fac2cfdbccf187487b1a17e")
set(_zstd_dictionary_evaluation_patched
    "ccbb084aac45ecf3d31e15e1e0a2102efe8e429e8e3fada2afd9ed728946bb02")
set(_zstd_evaluation_sum_original
    [=[/**
 * Returns the sum of the sample sizes.
 */
size_t COVER_sum(const size_t *samplesSizes, unsigned nbSamples) {
  size_t sum = 0;
  unsigned i;
  for (i = 0; i < nbSamples; ++i) {
    sum += samplesSizes[i];
  }
  return sum;
}]=])
string(
  CONCAT _zstd_evaluation_sum_checked
         "/* Purpose: Sum live sample sizes without wra"
         "pping the declared byte extent.\n"
         " * Inputs: A live array of nbSamples sizes; z"
         "ero samples may use a null array.\n"
         " * Outputs: Returns the sum or SIZE_MAX, whic"
         "h existing input-size limits reject. */\n"
         "size_t COVER_sum(const size_t *samplesSizes, "
         "unsigned nbSamples) {\n"
         "  size_t sum = 0;\n"
         "  unsigned i;\n"
         "  if ((nbSamples != 0 && samplesSizes == NULL"
         ") ||\n"
         "      nbSamples > (size_t)PTRDIFF_MAX / sizeo"
         "f(size_t)) return (size_t)-1;\n"
         "  for (i = 0; i < nbSamples; ++i) {\n"
         "    if (samplesSizes[i] > (size_t)-1 - sum) r"
         "eturn (size_t)-1;\n"
         "    sum += samplesSizes[i];\n"
         "  }\n"
         "  return sum;\n"
         "}")
