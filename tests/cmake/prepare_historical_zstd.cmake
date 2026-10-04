# Purpose: Reconstruct exact historical migration inputs without retaining
# obsolete production generators. Inputs: HISTORICAL_ROOT is an immutable Git
# snapshot; FIXTURE_ROOT is private source storage; STAGE selects one previously
# published boundary. Outputs: Hash-verified source fixtures, never compiled
# code.
if(NOT DEFINED HISTORICAL_ROOT
   OR NOT DEFINED FIXTURE_ROOT
   OR NOT DEFINED STAGE)
  message(FATAL_ERROR "Historical migration fixture inputs are required")
endif()
include("${HISTORICAL_ROOT}/cmake/PatchZstdLegacy.cmake")
if(STAGE STREQUAL "preowner" OR STAGE STREQUAL "complete")
  foreach(
    part IN
    ITEMS LegacyOwnedBuffers
          DictionaryEvaluation
          TableGeometry
          CoverWorkGroup
          AlgorithmProgress
          HeaderComponents
          LegacyOwnedDecoder)
    include("${HISTORICAL_ROOT}/cmake/Zstd${part}.cmake")
  endforeach()
  superzip_patch_zstd_base("${FIXTURE_ROOT}")
  superzip_patch_zstd_legacy_owned_buffers("${FIXTURE_ROOT}")
  superzip_patch_zstd_dictionary_evaluation("${FIXTURE_ROOT}")
  superzip_patch_zstd_table_geometry("${FIXTURE_ROOT}")
  superzip_patch_zstd_work_group("${FIXTURE_ROOT}")
  superzip_patch_zstd_algorithm_progress("${FIXTURE_ROOT}")
  superzip_patch_zstd_header_components("${FIXTURE_ROOT}")
  if(STAGE STREQUAL "complete")
    superzip_patch_zstd_legacy_owned_decoder("${FIXTURE_ROOT}")
  endif()
elseif(
  STAGE STREQUAL "literals"
  OR STAGE STREQUAL "stream"
  OR STAGE STREQUAL "history")
  superzip_patch_zstd_foundation("${FIXTURE_ROOT}")
  superzip_patch_zstd_legacy_literals("${FIXTURE_ROOT}")
  if(NOT STAGE STREQUAL "literals")
    superzip_patch_zstd_legacy_stream("${FIXTURE_ROOT}")
  endif()
  if(STAGE STREQUAL "history")
    include("${HISTORICAL_ROOT}/cmake/ZstdLegacyHistory.cmake")
    superzip_patch_zstd_legacy_history("${FIXTURE_ROOT}")
  endif()
else()
  message(FATAL_ERROR "Unknown historical migration fixture stage")
endif()
