# Purpose: Build the canonical pinned Windows Zstandard library for product or
# isolated validation. Inputs: source_root owns the identity-verified patched
# dependency. Outputs: Declares libzstd_shared and returns its include root.
function(superzip_add_zstd_library source_root)
  # Build the upstream C library without its CLI/test/assembler project. MSVC
  # uses the portable decoder; upstream disables its GNU assembly here.
  set(library_directory "${source_root}/lib")
  file(
    GLOB
    library_sources
    CONFIGURE_DEPENDS
    "${library_directory}/common/*.c"
    "${library_directory}/compress/*.c"
    "${library_directory}/decompress/*.c"
    "${library_directory}/dictBuilder/*.c"
    "${library_directory}/legacy/*.c")
  set(resource_directory "${source_root}/build/VS2010/libzstd-dll")
  add_library(
    libzstd_shared SHARED
    ${library_sources}
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCoverSelection.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdCoverWorkGroup.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyBuffers.cpp"
    "${resource_directory}/libzstd-dll.rc")
  target_compile_features(libzstd_shared PRIVATE cxx_std_20)
  target_include_directories(
    libzstd_shared PRIVATE "${library_directory}"
                           "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_compile_definitions(
    libzstd_shared
    PRIVATE ZSTD_MULTITHREAD
            ZSTD_DLL_EXPORT=1
            ZSTD_DISABLE_ASM
            ZSTD_HEAPMODE=0
            ZSTD_LEGACY_SUPPORT=5
            XXH_NAMESPACE=ZSTD_
            _CRT_SECURE_NO_WARNINGS)
  set_target_properties(libzstd_shared PROPERTIES OUTPUT_NAME libzstd)
  set(SUPERZIP_ZSTD_LIBRARY_DIR
      "${library_directory}"
      PARENT_SCOPE)
endfunction()
