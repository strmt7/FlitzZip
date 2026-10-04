# Canonical dependency contracts shared by the product build and isolated
# sanitizer validation. Inputs: SUPERZIP_SOURCE_ROOT, SUPERZIP_ZSTD_LIBRARY_DIR
# and libzstd_shared. Outputs: Declares the same five dependency test targets
# without duplicating decoder algorithms. Guard-page regressions call the exact
# DLL built for the product, including dictionary builders that are exported but
# not used by archive dispatch.
if(NOT DEFINED SUPERZIP_SOURCE_ROOT
   OR NOT TARGET libzstd_shared
   OR NOT EXISTS "${SUPERZIP_ZSTD_LIBRARY_DIR}/zstd.h")
  message(FATAL_ERROR "Canonical Zstandard tests require the library target "
                      "and returned source include root")
endif()
include("${SUPERZIP_SOURCE_ROOT}/tests/zstd/RawBlockProbe.cmake")
set(_raw_block_probe "${CMAKE_CURRENT_BINARY_DIR}/tests/zstd_raw_block_probe.c")
superzip_generate_zstd_raw_block_probe(
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/compress/zstd_compress_internal.h"
  "${_raw_block_probe}")
include("${SUPERZIP_SOURCE_ROOT}/tests/zstd/HuffmanTableProbe.cmake")
set(_huffman_probe
    "${CMAKE_CURRENT_BINARY_DIR}/tests/zstd_huffman_table_probe.c")
superzip_generate_zstd_huffman_probe(
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/decompress/huf_decompress.c"
  "${_huffman_probe}")
add_executable(
  superzip_zstd_bounds_tests
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_main.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_bounds.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_huffman_table.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_fse_table.cpp"
  "${_raw_block_probe}"
  "${_huffman_probe}")
target_include_directories(
  superzip_zstd_bounds_tests
  PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/cpp"
          "${SUPERZIP_SOURCE_ROOT}/tests/zstd" "${SUPERZIP_ZSTD_LIBRARY_DIR}")
target_compile_definitions(
  superzip_zstd_bounds_tests PRIVATE ZSTD_DLL_IMPORT=1 NOMINMAX
                                     WIN32_LEAN_AND_MEAN)
target_link_libraries(superzip_zstd_bounds_tests PRIVATE libzstd_shared)
superzip_copy_zstd_runtime(superzip_zstd_bounds_tests)
add_test(NAME superzip_zstd_bounds_tests COMMAND superzip_zstd_bounds_tests)
set_tests_properties(superzip_zstd_bounds_tests PROPERTIES TIMEOUT 60)
# Observe finalizer extents in the exact generated selection helper while
# forwarding successful calls to the product DLL; allocator faults are serial.
add_library(
  superzip_zstd_cover_probe OBJECT
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder/cover.c"
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder/fastcover.c")
target_include_directories(
  superzip_zstd_cover_probe
  PRIVATE "${SUPERZIP_SOURCE_ROOT}/cmake" "${SUPERZIP_SOURCE_ROOT}/tests/zstd"
          "${SUPERZIP_ZSTD_LIBRARY_DIR}")
target_compile_definitions(
  superzip_zstd_cover_probe
  PRIVATE ZDICT_finalizeDictionary=sz_checked_finalize
          SUPERZIP_COVER_FAULT_ALLOCATIONS _CRT_SECURE_NO_WARNINGS)
set(_cover_allocator_header
    "${SUPERZIP_SOURCE_ROOT}/tests/zstd/intercept_allocations.h")
if(MSVC)
  target_compile_options(superzip_zstd_cover_probe
                         PRIVATE "/FI${_cover_allocator_header}")
else()
  target_compile_options(superzip_zstd_cover_probe
                         PRIVATE -include "${_cover_allocator_header}")
endif()
# Visual Studio cannot represent target-wide C flags separately in a mixed C/C++
# object target. Keep the two instrumented languages in distinct targets.
add_library(
  superzip_zstd_cover_owner_probe OBJECT
  "${SUPERZIP_SOURCE_ROOT}/cmake/ZstdCoverSelection.cpp"
  "${SUPERZIP_SOURCE_ROOT}/cmake/ZstdCoverWorkGroup.cpp")
target_include_directories(
  superzip_zstd_cover_owner_probe PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/zstd"
                                          "${SUPERZIP_ZSTD_LIBRARY_DIR}")
target_compile_features(superzip_zstd_cover_owner_probe PRIVATE cxx_std_20)
target_compile_definitions(
  superzip_zstd_cover_owner_probe
  PRIVATE ZDICT_finalizeDictionary=sz_checked_finalize
          SUPERZIP_COVER_FAULT_ALLOCATIONS _CRT_SECURE_NO_WARNINGS)
# Direct merger controls compile the exact generated type/helper region; the
# bridge marshals bounded fixtures without publishing a private API.
include("${SUPERZIP_SOURCE_ROOT}/tests/zstd/DictionaryMergeProbe.cmake")
set(_dictionary_merge_probe
    "${CMAKE_CURRENT_BINARY_DIR}/tests/zstd_dictionary_merge_probe.c")
superzip_generate_zstd_dictionary_probe(
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder/zdict.c"
  "${_dictionary_merge_probe}")
add_executable(
  superzip_zstd_cover_selection_tests
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_main.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_cover_selection.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_work_group.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_dictionary_merge.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/zstd/fault_allocator.cpp"
  "${_dictionary_merge_probe}"
  $<TARGET_OBJECTS:superzip_zstd_cover_probe>
  $<TARGET_OBJECTS:superzip_zstd_cover_owner_probe>
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder/divsufsort.c"
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/pool.c"
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/threading.c")
target_include_directories(
  superzip_zstd_cover_selection_tests
  PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/cpp" "${SUPERZIP_SOURCE_ROOT}/cmake"
          "${SUPERZIP_SOURCE_ROOT}/tests/zstd" "${SUPERZIP_ZSTD_LIBRARY_DIR}"
          "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder")
target_compile_definitions(
  superzip_zstd_cover_selection_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
                                              _CRT_SECURE_NO_WARNINGS)
foreach(target IN
        ITEMS superzip_zstd_cover_probe superzip_zstd_cover_owner_probe
              superzip_zstd_cover_selection_tests)
  target_compile_definitions(
    ${target}
    PRIVATE ZSTD_MULTITHREAD
            NOMINMAX
            COVER_createWorkGroup=sz_fault_createWorkGroup
            COVER_workGroupBest=sz_fault_workGroupBest
            COVER_workGroupPool=sz_fault_workGroupPool
            COVER_copyWorkDictionary=sz_fault_copyWorkDictionary
            COVER_prepareWorkContext=sz_fault_prepareWorkContext
            COVER_commitWorkContext=sz_fault_commitWorkContext
            COVER_finishWorkContext=sz_fault_finishWorkContext
            COVER_releaseWorkGroup=sz_fault_releaseWorkGroup)
endforeach()
target_link_libraries(superzip_zstd_cover_selection_tests
                      PRIVATE libzstd_shared)
superzip_copy_zstd_runtime(superzip_zstd_cover_selection_tests)
add_test(NAME superzip_zstd_cover_selection_tests
         COMMAND superzip_zstd_cover_selection_tests)
set_tests_properties(superzip_zstd_cover_selection_tests PROPERTIES TIMEOUT 60)
# Instrumented translation units have distinct build paths so analysis can
# distinguish their allocator substitutions from production definitions. Require
# byte-identical complete sources; neither implementation includes nor a
# separate decoder algorithm participates in the fault tests.
set(_legacy_fault_source_dir
    "${CMAKE_CURRENT_BINARY_DIR}/tests/zstd_legacy_fault_sources")
set(_legacy_fault_sources)
foreach(legacy_version IN ITEMS v05 v06 v07)
  set(_legacy_original
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy/zstd_${legacy_version}.c")
  set(_legacy_instrumented
      "${_legacy_fault_source_dir}/zstd_${legacy_version}.c")
  file(SHA256 "${_legacy_original}" _legacy_original_hash)
  configure_file("${_legacy_original}" "${_legacy_instrumented}" COPYONLY)
  file(SHA256 "${_legacy_instrumented}" _legacy_instrumented_hash)
  if(NOT _legacy_original_hash STREQUAL _legacy_instrumented_hash)
    message(FATAL_ERROR "Legacy fault-test source identity mismatch")
  endif()
  list(APPEND _legacy_fault_sources "${_legacy_instrumented}")
endforeach()
add_library(superzip_zstd_legacy_fault_objects OBJECT ${_legacy_fault_sources})
target_include_directories(
  superzip_zstd_legacy_fault_objects
  PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/zstd" "${SUPERZIP_ZSTD_LIBRARY_DIR}"
          "${SUPERZIP_SOURCE_ROOT}/cmake" "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy")
target_compile_definitions(
  superzip_zstd_legacy_fault_objects
  PRIVATE ZSTD_LEGACY_SUPPORT=5 XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM
          _CRT_SECURE_NO_WARNINGS SUPERZIP_ZSTD_BUFFER_PROBES)
set(_legacy_allocator_header
    "${SUPERZIP_SOURCE_ROOT}/tests/zstd/intercept_allocations.h")
if(MSVC)
  target_compile_options(superzip_zstd_legacy_fault_objects
                         PRIVATE "/FI${_legacy_allocator_header}")
else()
  target_compile_options(superzip_zstd_legacy_fault_objects
                         PRIVATE -include "${_legacy_allocator_header}")
endif()
add_library(superzip_zstd_legacy_buffer_probe OBJECT
            "${SUPERZIP_SOURCE_ROOT}/cmake/ZstdLegacyBuffers.cpp")
target_include_directories(superzip_zstd_legacy_buffer_probe
                           PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/zstd")
target_compile_features(superzip_zstd_legacy_buffer_probe PRIVATE cxx_std_20)
target_compile_definitions(superzip_zstd_legacy_buffer_probe
                           PRIVATE SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS)
add_executable(
  superzip_zstd_legacy_tests
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_main.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_legacy_failures.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_legacy_history.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/cpp/test_zstd_legacy_buffers.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/zstd/fault_allocator.cpp"
  "${SUPERZIP_SOURCE_ROOT}/tests/zstd/legacy_driver.c"
  $<TARGET_OBJECTS:superzip_zstd_legacy_fault_objects>
  $<TARGET_OBJECTS:superzip_zstd_legacy_buffer_probe>
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/error_private.c"
  "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/xxhash.c")
target_include_directories(
  superzip_zstd_legacy_tests
  PRIVATE "${SUPERZIP_SOURCE_ROOT}/tests/cpp" "${SUPERZIP_SOURCE_ROOT}/cmake"
          "${SUPERZIP_SOURCE_ROOT}/tests/zstd" "${SUPERZIP_ZSTD_LIBRARY_DIR}")
target_compile_definitions(
  superzip_zstd_legacy_tests
  PRIVATE ZSTD_LEGACY_SUPPORT=5 XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM
          _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
# Production and fault owners have distinct C symbols. This prevents duplicate
# definitions from obscuring the tested ownership graph in linked analysis.
foreach(target IN
        ITEMS superzip_zstd_legacy_fault_objects
              superzip_zstd_legacy_buffer_probe superzip_zstd_legacy_tests)
  target_compile_definitions(
    ${target}
    PRIVATE ZBUFF_createOwnedBuffers=sz_fault_createOwnedBuffers
            ZBUFF_reserveOwnedBuffers=sz_fault_reserveOwnedBuffers
            ZBUFF_viewOwnedBuffers=sz_fault_viewOwnedBuffers
            ZBUFF_releaseOwnedBuffers=sz_fault_releaseOwnedBuffers
            ZBUFF_copyBytes=sz_fault_copyBytes)
endforeach()
add_test(NAME superzip_zstd_legacy_tests COMMAND superzip_zstd_legacy_tests)
add_test(
  NAME superzip_zstd_legacy_patch
  COMMAND
    "${CMAKE_COMMAND}" "-DREPO_ROOT=${SUPERZIP_SOURCE_ROOT}"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P
    "${SUPERZIP_SOURCE_ROOT}/tests/cmake/test_zstd_legacy_patch.cmake")
string(CONCAT _zstd_header_contract "${SUPERZIP_SOURCE_ROOT}/tests/cmake/"
              "test_zstd_header_contracts.cmake")
add_test(
  NAME superzip_zstd_header_contracts
  COMMAND
    "${CMAKE_COMMAND}" "-DREPO_ROOT=${SUPERZIP_SOURCE_ROOT}"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}" -P
    "${_zstd_header_contract}")
set_tests_properties(superzip_zstd_legacy_tests PROPERTIES TIMEOUT 60)
# The expanded exact-revision migration matrix took 59 seconds in the product
# build; retain bounded headroom for the shared-host and instrumented consumers.
set_tests_properties(superzip_zstd_legacy_patch PROPERTIES TIMEOUT 120)
set_tests_properties(superzip_zstd_header_contracts PROPERTIES TIMEOUT 150)
