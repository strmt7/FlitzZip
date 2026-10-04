# Purpose: Separate a completed prefix search from its remaining comparison
# budget. Inputs: Exact preceding match-search source and flavor. Outputs:
# Preserves comparison consumption and dictionary dispatch while removing
# counter mutation.
function(superzip_rewrite_zstd_match_progress content flavor output)
  set(prefix "")
  if(flavor STREQUAL lazy)
    string(CONCAT declaration "    U32          nbCompares = "
                  "1U << cParams->searchLog;")
    string(
      CONCAT old_stop
             "                        nbCompares = 0; "
             "/* in addition to avoiding checking any\n"
             "                                         * further in this loop, "
             "make sure we\n"
             "                                         * skip checking in the "
             "dictionary. */")
    set(new_stop "                        prefixSearchComplete = 1;")
  else()
    set(declaration "    U32 nbCompares = 1U << cParams->searchLog;")
    string(CONCAT old_stop "if (dictMode == ZSTD_dictMatchState) "
                  "nbCompares = 0; /* break should also skip searching dms */")
    set(new_stop
        "if (dictMode == ZSTD_dictMatchState) prefixSearchComplete = 1;")
    # Only the match-collecting function contains the reported early exit.
    string(FIND "${content}" "ZSTD_insertBtAndGetAllMatches (" begin)
    if(begin LESS 0)
      message(FATAL_ERROR "Zstandard match-progress boundary mismatch")
    endif()
    string(SUBSTRING "${content}" 0 ${begin} prefix)
    string(SUBSTRING "${content}" ${begin} -1 content)
  endif()
  string(
    CONCAT
      checked_declaration "${declaration}\n"
      "    /* Search termination does not consume or reset the budget. */\n"
      "    int prefixSearchComplete = 0;")
  string(REPLACE "${declaration}" "${checked_declaration}" content "${content}")
  string(REPLACE "${old_stop}" "${new_stop}" content "${content}")
  string(CONCAT dictionary_gate
                "if (dictMode == ZSTD_dictMatchState && nbCompares "
                "&& !prefixSearchComplete)")
  string(REPLACE "if (dictMode == ZSTD_dictMatchState && nbCompares)"
                 "${dictionary_gate}" content "${content}")
  set(content "${prefix}${content}")
  if(flavor STREQUAL lazy)
    string(CONCAT signature "static\nZSTD_ALLOW_POINTER_OVERFLOW_ATTR\n"
                  "size_t ZSTD_DUBT_findBestMatch(")
  else()
    string(CONCAT signature "FORCE_INLINE_TEMPLATE\n"
                  "ZSTD_ALLOW_POINTER_OVERFLOW_ATTR\nU32\n"
                  "ZSTD_insertBtAndGetAllMatches (")
  endif()
  string(
    CONCAT contract
           "/* Purpose: Search prefix and attached dictionary matches "
           "within the comparison budget.\n"
           " * Inputs: Initialized match state and live source, window "
           "and dictionary extents.\n"
           " * Outputs: Match results and tree updates; completed prefix "
           "search skips dictionary lookup without resetting the budget. */\n")
  string(REPLACE "${signature}" "${contract}${signature}" content "${content}")
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Replace intertwined suffix-group loop control with a bounded rank
# stage. Inputs: Exact preceding suffix sorter. Outputs: Checked group progress
# and failure propagation to both suffix-array and BWT consumers.
function(superzip_rewrite_zstd_suffix_progress content output)
  string(FIND "${content}" "    /* Compute ranks of type B* substrings. */"
              begin)
  string(FIND "${content}" "    /* Construct the inverse suffix array" end)
  string(FIND "${content}" "/* Sorts suffixes of type B*. */" helper_begin)
  if(begin LESS 0
     OR end LESS_EQUAL begin
     OR helper_begin LESS 0)
    message(FATAL_ERROR "Zstandard suffix-progress boundary mismatch")
  endif()
  string(SUBSTRING "${content}" 0 ${begin} prefix)
  string(SUBSTRING "${content}" ${end} -1 suffix)
  string(CONCAT checked "    /* Compute bounded ranks of type B* groups. */\n"
                "    if (!compute_typeBstar_ranks(SA, ISAb, m)) return -1;\n\n")
  set(content "${prefix}${checked}${suffix}")
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdSuffixRanks.c" helper)
  string(REGEX REPLACE "\n+$" "" helper "${helper}")
  string(REPLACE "/* Sorts suffixes of type B*. */"
                 "${helper}\n\n/* Sorts suffixes of type B*. */" content
                 "${content}")
  string(CONCAT suffix_consumer "    if (m < 0) err = -1;\n"
                "    else construct_SA(T, SA, bucket_A, bucket_B, n, m);")
  string(REPLACE "    construct_SA(T, SA, bucket_A, bucket_B, n, m);"
                 "${suffix_consumer}" content "${content}")
  string(CONCAT bwt_consumer
                "    if (m < 0) {\n      pidx = -1;\n    } else {\n"
                "    if (num_indexes == NULL || indexes == NULL) {")
  string(REPLACE "    if (num_indexes == NULL || indexes == NULL) {"
                 "${bwt_consumer}" content "${content}")
  string(REPLACE "    pidx += 1;" "    pidx += 1;\n    }" content "${content}")
  string(
    CONCAT rank_contract
           "/* Purpose: Sort and rank B-star suffix groups in the "
           "bounded suffix workspace.\n"
           " * Inputs: Live n-byte corpus and n-element workspace, "
           "with initialized bucket allocations.\n"
           " * Outputs: B-star count or -1 for malformed rank state; "
           "preserves valid group ordering. */\n")
  string(REPLACE "/* Sorts suffixes of type B*. */"
                 "${rank_contract}/* Sorts suffixes of type B*. */" content
                 "${content}")
  foreach(consumer IN ITEMS divsufsort divbwt)
    string(
      CONCAT consumer_contract
             "/* Purpose: Publish ${consumer} results after bounded "
             "suffix ranking succeeds.\n"
             " * Inputs: Caller-owned corpus and workspace/output "
             "extents under the existing C API.\n"
             " * Outputs: Existing result or negative error; "
             "rank failure cannot enter construction. */\n")
    string(REPLACE "int\n${consumer}(" "${consumer_contract}int\n${consumer}("
                   content "${content}")
  endforeach()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

# Purpose: Publish behavior-preserving bounded algorithm progress for exact
# sources. Inputs: Pinned extracted dependency after earlier repairs. Outputs:
# Complete identity-verified source revisions or rejection; preserves
# provenance.
function(superzip_patch_zstd_algorithm_progress source_dir)
  foreach(flavor IN ITEMS lazy opt suffix)
    set(key "_zstd_algorithm_progress_${flavor}")
    set(source "${source_dir}/${${key}_path}")
    file(SHA256 "${source}" actual)
    if(actual STREQUAL "${${key}_patched}")
      continue()
    endif()
    if(NOT actual STREQUAL "${${key}_original}")
      message(FATAL_ERROR "Zstandard patch source identity mismatch: ${source}")
    endif()
    file(READ "${source}" content)
    if(flavor STREQUAL suffix)
      superzip_rewrite_zstd_suffix_progress("${content}" content)
    else()
      superzip_rewrite_zstd_match_progress("${content}" "${flavor}" content)
    endif()
    superzip_write_verified_zstd_patch("${source}" "${actual}"
                                       "${${key}_patched}" "${content}")
  endforeach()
endfunction()

set(_zstd_algorithm_progress_lazy_path "lib/compress/zstd_lazy.c")
set(_zstd_algorithm_progress_lazy_original
    "8f44e994583aacf9ed95004614d7fada1f54f470473d52080fc10836ba3faffe")
set(_zstd_algorithm_progress_opt_path "lib/compress/zstd_opt.c")
set(_zstd_algorithm_progress_opt_original
    "4952125ea426e028d0aba6c94175debe49d941e41bc873a52f66d868399b0d32")
set(_zstd_algorithm_progress_suffix_path "lib/dictBuilder/divsufsort.c")
set(_zstd_algorithm_progress_suffix_original
    "a2d8d333895d0e54db6af52cc618c60007a0e919a42e58523a3a4dc4d3ddfd9b")
set(_zstd_algorithm_progress_lazy_patched
    "73a90f893fa52646bd22b292d9c721d0f07891115fabd1700ab50badd672b29f")
set(_zstd_algorithm_progress_opt_patched
    "d24ba148e9dbeeb0721cbc3a7b5a77dddc7ec8a17df93916e18403d40aed975b")
set(_zstd_algorithm_progress_suffix_patched
    "55e83c7ec4221638a9f5333893ed44441d9c323e9567e68d03ed9a24341bbfad")
