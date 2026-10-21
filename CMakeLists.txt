cmake_minimum_required(VERSION 3.24)

project(
  FlitzZip
  VERSION 0.8.0
  LANGUAGES C CXX)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FLITZZIP_ENABLE_HIP "Build AMD HIP acceleration backend" ON)
option(FLITZZIP_BUILD_GUI "Build native Windows GUI" ON)
option(FLITZZIP_BUILD_TESTS "Build C++ test binaries" ON)
option(FLITZZIP_BUILD_FUZZERS "Build libFuzzer targets for parser hardening"
       OFF)
set(FLITZZIP_HIP_ARCH
    "release"
    CACHE STRING "HIP gfx target, comma-separated targets, or release preset")
string(CONCAT FLITZZIP_VCVARS_VERSION_HELP
              "Optional MSVC toolset version used by hipcc on Windows; "
              "empty auto-selects a compatible installed toolset when possible")
set(FLITZZIP_VCVARS_VERSION
    ""
    CACHE STRING "${FLITZZIP_VCVARS_VERSION_HELP}")
set(FLITZZIP_PACKAGE_VERSION
    "${PROJECT_VERSION}"
    CACHE STRING
          "Display version used in package filenames and app diagnostics")
string(
  CONCAT FLITZZIP_PACKAGE_VERSION_REGEX
         "^(0|[1-9][0-9]*)\\."
         "(0|[1-9][0-9]*)\\."
         "(0|[1-9][0-9]*)"
         "(-((0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)"
         "(\\.(0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$")
if(NOT FLITZZIP_PACKAGE_VERSION MATCHES "${FLITZZIP_PACKAGE_VERSION_REGEX}")
  message(
    FATAL_ERROR
      "FLITZZIP_PACKAGE_VERSION must be SemVer without build metadata, "
      "for example 0.1.0 or 0.1.0-beta.1.")
endif()
set(FLITZZIP_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(FLITZZIP_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(FLITZZIP_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
string(
  CONCAT FLITZZIP_PACKAGE_NUMERIC_VERSION "${FLITZZIP_PACKAGE_VERSION_MAJOR}."
         "${FLITZZIP_PACKAGE_VERSION_MINOR}."
         "${FLITZZIP_PACKAGE_VERSION_PATCH}")
string(CONCAT FLITZZIP_MSI_INSTALL_SCOPE_HELP
              "WiX MSI install scope: perMachine installs under Program Files "
              "and requires elevation; perUser installs under LocalAppData")
set(FLITZZIP_MSI_INSTALL_SCOPE
    "perMachine"
    CACHE STRING "${FLITZZIP_MSI_INSTALL_SCOPE_HELP}")
set_property(CACHE FLITZZIP_MSI_INSTALL_SCOPE PROPERTY STRINGS perUser
                                                       perMachine)
if(NOT FLITZZIP_MSI_INSTALL_SCOPE STREQUAL "perUser"
   AND NOT FLITZZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
  message(
    FATAL_ERROR "FLITZZIP_MSI_INSTALL_SCOPE must be perUser or perMachine.")
endif()
string(
  CONCAT FLITZZIP_MSI_PRODUCT_IDENTITY_HELP
         "Unique MSI package/build identity included in ProductCode "
         "derivation; release workflows set this to the GitHub run identity "
         "so same-version release replacements perform a major upgrade.")
set(FLITZZIP_MSI_PRODUCT_IDENTITY
    ""
    CACHE STRING "${FLITZZIP_MSI_PRODUCT_IDENTITY_HELP}")
if(NOT FLITZZIP_MSI_PRODUCT_IDENTITY)
  string(TIMESTAMP FLITZZIP_MSI_PRODUCT_IDENTITY "local-%Y%m%d%H%M%S" UTC)
endif()
if(NOT FLITZZIP_MSI_PRODUCT_IDENTITY MATCHES "^[A-Za-z0-9_.:-]+$")
  message(
    FATAL_ERROR
      "FLITZZIP_MSI_PRODUCT_IDENTITY must contain only letters, digits, "
      "underscore, dot, colon, or hyphen.")
endif()
set(FLITZZIP_WIX_UPGRADE_GUID "8E0D80F6-859D-4FE9-B082-F0D8048A8B57")
string(
  CONCAT FLITZZIP_WIX_PRODUCT_CODE_SEED "FlitzZip MSI ProductCode "
         "${FLITZZIP_PACKAGE_NUMERIC_VERSION} "
         "${FLITZZIP_MSI_INSTALL_SCOPE} " "${FLITZZIP_MSI_PRODUCT_IDENTITY}")
string(SHA256 FLITZZIP_WIX_PRODUCT_CODE_HASH
              "${FLITZZIP_WIX_UPGRADE_GUID}|${FLITZZIP_WIX_PRODUCT_CODE_SEED}")
string(SUBSTRING "${FLITZZIP_WIX_PRODUCT_CODE_HASH}" 0 8
                 FLITZZIP_WIX_PRODUCT_GUID_A)
string(SUBSTRING "${FLITZZIP_WIX_PRODUCT_CODE_HASH}" 8 4
                 FLITZZIP_WIX_PRODUCT_GUID_B)
string(SUBSTRING "${FLITZZIP_WIX_PRODUCT_CODE_HASH}" 12 4
                 FLITZZIP_WIX_PRODUCT_GUID_C)
string(SUBSTRING "${FLITZZIP_WIX_PRODUCT_CODE_HASH}" 16 4
                 FLITZZIP_WIX_PRODUCT_GUID_D)
string(SUBSTRING "${FLITZZIP_WIX_PRODUCT_CODE_HASH}" 20 12
                 FLITZZIP_WIX_PRODUCT_GUID_E)
string(CONCAT FLITZZIP_WIX_PRODUCT_GUID "${FLITZZIP_WIX_PRODUCT_GUID_A}-"
              "${FLITZZIP_WIX_PRODUCT_GUID_B}-${FLITZZIP_WIX_PRODUCT_GUID_C}-"
              "${FLITZZIP_WIX_PRODUCT_GUID_D}-${FLITZZIP_WIX_PRODUCT_GUID_E}")
string(TOUPPER "${FLITZZIP_WIX_PRODUCT_GUID}" FLITZZIP_WIX_PRODUCT_GUID)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lz4 STATIC third_party/lz4/lz4.c)
target_include_directories(lz4 PUBLIC third_party/lz4)

add_library(
  miniz STATIC
  src/core/miniz_checksum.cpp third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_include_directories(miniz PRIVATE src)
target_link_libraries(miniz PRIVATE flitzzip_checksum)
target_compile_definitions(
  miniz
  PRIVATE _CRT_SECURE_NO_WARNINGS USE_EXTERNAL_MZCRC
          MZ_ZIP_MAX_READER_FILES=250000U
          MZ_ZIP_MAX_READER_METADATA_SIZE=268435456ULL)

add_library(
  bzip2 STATIC
  third_party/bzip2/blocksort.c
  third_party/bzip2/bz_internal_error.c
  third_party/bzip2/bzlib.c
  third_party/bzip2/compress.c
  third_party/bzip2/crctable.c
  third_party/bzip2/decompress.c
  third_party/bzip2/huffman.c
  third_party/bzip2/randtable.c)
target_include_directories(bzip2 PUBLIC third_party/bzip2)
target_compile_definitions(bzip2 PUBLIC BZ_NO_STDIO)
target_compile_definitions(bzip2 PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(
  xz_embedded STATIC
  third_party/xz_embedded/linux/lib/xz/xz_crc32.c
  third_party/xz_embedded/linux/lib/xz/xz_crc64.c
  third_party/xz_embedded/linux/lib/xz/xz_dec_lzma2.c
  third_party/xz_embedded/linux/lib/xz/xz_dec_stream.c)
target_include_directories(
  xz_embedded
  PUBLIC third_party/xz_embedded/linux/include/linux
         third_party/xz_embedded/userspace
  PRIVATE third_party/xz_embedded/linux/lib/xz)
target_compile_definitions(xz_embedded PRIVATE XZ_DEC_DYNALLOC
                                               _CRT_SECURE_NO_WARNINGS)

add_library(
  lzma_sdk_7z STATIC
  third_party/lzma_sdk/C/7zArcIn.c
  third_party/lzma_sdk/C/7zBuf.c
  third_party/lzma_sdk/C/7zBuf2.c
  third_party/lzma_sdk/C/7zCrc.c
  third_party/lzma_sdk/C/7zCrcOpt.c
  third_party/lzma_sdk/C/7zDec.c
  third_party/lzma_sdk/C/7zStream.c
  third_party/lzma_sdk/C/Bcj2.c
  third_party/lzma_sdk/C/Bra.c
  third_party/lzma_sdk/C/Bra86.c
  third_party/lzma_sdk/C/BraIA64.c
  third_party/lzma_sdk/C/CpuArch.c
  third_party/lzma_sdk/C/CpuArchByteAccess.cpp
  third_party/lzma_sdk/C/Delta.c
  third_party/lzma_sdk/C/Lzma2Dec.c
  third_party/lzma_sdk/C/LzmaDec.c
  third_party/lzma_sdk/C/Ppmd7.c
  third_party/lzma_sdk/C/Ppmd7Dec.c)
target_include_directories(lzma_sdk_7z PUBLIC third_party/lzma_sdk/C)
target_compile_features(lzma_sdk_7z PRIVATE cxx_std_20)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL
                                           "MSVC")
  include(CheckIPOSupported)
  check_ipo_supported(
    RESULT sdk_wire_ipo_supported
    OUTPUT sdk_wire_ipo_error
    LANGUAGES C CXX)
  if(NOT sdk_wire_ipo_supported)
    message(FATAL_ERROR "SDK wire access requires IPO: ${sdk_wire_ipo_error}")
  endif()
  set_property(TARGET lzma_sdk_7z PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE
                                           TRUE)
  # The C bridge requires IPO; tell consumers before linking so MSVC does not
  # discover /GL inputs and restart the link. Release already disables ordinary
  # incremental linking, and this preserves the same optimized output contract.
  target_link_options(lzma_sdk_7z INTERFACE "$<$<CONFIG:Release>:/LTCG>")
endif()
target_compile_definitions(lzma_sdk_7z PUBLIC Z7_EXTRACT_ONLY Z7_PPMD_SUPPORT)
target_compile_definitions(lzma_sdk_7z PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(lzma_sdk_7z PRIVATE /wd4244 /wd4267 /wd4334)
else()
  target_compile_options(lzma_sdk_7z PRIVATE -Wno-conversion
                                             -Wno-sign-conversion)
endif()

add_library(flitzzip_checksum STATIC src/core/checksum.cpp)
target_include_directories(flitzzip_checksum PUBLIC src)
target_link_libraries(flitzzip_checksum PRIVATE lzma_sdk_7z)
if(MSVC)
  target_compile_options(flitzzip_checksum PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(flitzzip_checksum PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(
  lhasa STATIC
  third_party/lhasa/lib/crc16.c
  third_party/lhasa/lib/ext_header.c
  third_party/lhasa/lib/lha_decoder.c
  third_party/lhasa/lib/lha_endian.c
  third_party/lhasa/lib/lha_file_header.c
  third_party/lhasa/lib/lha_input_stream.c
  third_party/lhasa/lib/lha_basic_reader.c
  third_party/lhasa/lib/lha_reader.c
  third_party/lhasa/lib/macbinary.c
  third_party/lhasa/lib/null_decoder.c
  third_party/lhasa/lib/lh1_decoder.c
  third_party/lhasa/lib/lh5_decoder.c
  third_party/lhasa/lib/lh6_decoder.c
  third_party/lhasa/lib/lh7_decoder.c
  third_party/lhasa/lib/lhx_decoder.c
  third_party/lhasa/lib/lk7_decoder.c
  third_party/lhasa/lib/lz5_decoder.c
  third_party/lhasa/lib/lzs_decoder.c
  third_party/lhasa/lib/pm1_decoder.c
  third_party/lhasa/lib/pm2_decoder.c)
target_include_directories(
  lhasa
  PUBLIC third_party/lhasa/lib/public
  PRIVATE third_party/lhasa/lib)
target_compile_definitions(lhasa PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(lhasa PRIVATE /wd4244 /wd4267)
else()
  target_compile_options(lhasa PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()

set(FLITZZIP_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/upstream")
set(FLITZZIP_ZSTD_RUNTIME_PACKAGE_DIR "${FLITZZIP_UPSTREAM_DIR}/zstd/v1.5.7")
set(FLITZZIP_ZSTD_RUNTIME_PACKAGE_NAME "zstd-v1.5.7.zip")
set(FLITZZIP_ZSTD_RUNTIME_PACKAGE
    "${FLITZZIP_ZSTD_RUNTIME_PACKAGE_DIR}/${FLITZZIP_ZSTD_RUNTIME_PACKAGE_NAME}"
)
set(FLITZZIP_ZSTD_RUNTIME_PACKAGE_SHA256
    "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4")
set(FLITZZIP_ZSTD_RUNTIME_EXTRACT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/zstd-v1.5.7-source")
set(FLITZZIP_ZSTD_RUNTIME_SOURCE_DIR
    "${FLITZZIP_ZSTD_RUNTIME_EXTRACT_DIR}/zstd-1.5.7")
set(FLITZZIP_WIMLIB_RUNTIME_PACKAGE_DIR
    "${FLITZZIP_UPSTREAM_DIR}/wimlib/1.14.5")
set(FLITZZIP_WIMLIB_RUNTIME_PACKAGE_NAME "wimlib-1.14.5-windows-x86_64-bin.zip")
string(CONCAT FLITZZIP_WIMLIB_RUNTIME_PACKAGE
              "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE_DIR}/"
              "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE_NAME}")
set(FLITZZIP_WIMLIB_RUNTIME_PACKAGE_SHA256
    "2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265")
set(FLITZZIP_WIMLIB_RUNTIME_DLL_SHA256
    "ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495")
set(FLITZZIP_WIMLIB_RUNTIME_EXTRACT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/wimlib-1.14.5-win64-runtime")
set(FLITZZIP_WIMLIB_RUNTIME_DLL
    "${FLITZZIP_WIMLIB_RUNTIME_EXTRACT_DIR}/libwim-15.dll")
if(WIN32)
  if(NOT EXISTS "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}")
    message(FATAL_ERROR "Bundled Zstandard runtime package is missing: "
                        "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}")
  endif()
  file(SHA256 "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}"
       FLITZZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256)
  if(NOT FLITZZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL
     FLITZZIP_ZSTD_RUNTIME_PACKAGE_SHA256)
    message(FATAL_ERROR "Bundled Zstandard runtime package checksum mismatch: "
                        "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}")
  endif()
  set(FLITZZIP_ZSTD_RUNTIME_CMAKE
      "${FLITZZIP_ZSTD_RUNTIME_SOURCE_DIR}/build/cmake/CMakeLists.txt")
  if(NOT EXISTS "${FLITZZIP_ZSTD_RUNTIME_CMAKE}")
    file(MAKE_DIRECTORY "${FLITZZIP_ZSTD_RUNTIME_EXTRACT_DIR}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}"
      WORKING_DIRECTORY "${FLITZZIP_ZSTD_RUNTIME_EXTRACT_DIR}"
      RESULT_VARIABLE FLITZZIP_ZSTD_RUNTIME_EXTRACT_RESULT)
    if(NOT FLITZZIP_ZSTD_RUNTIME_EXTRACT_RESULT EQUAL 0)
      message(
        FATAL_ERROR "Failed to extract bundled Zstandard runtime package: "
                    "${FLITZZIP_ZSTD_RUNTIME_PACKAGE}")
    endif()
  endif()
  if(NOT EXISTS "${FLITZZIP_ZSTD_RUNTIME_CMAKE}")
    message(
      FATAL_ERROR "Pinned Zstandard source was not found after extraction: "
                  "${FLITZZIP_ZSTD_RUNTIME_SOURCE_DIR}")
  endif()
  include(cmake/PatchZstdLegacy.cmake)
  flitzzip_patch_zstd_legacy("${FLITZZIP_ZSTD_RUNTIME_SOURCE_DIR}")
  include(cmake/ZstdLibrary.cmake)
  flitzzip_add_zstd_library("${FLITZZIP_ZSTD_RUNTIME_SOURCE_DIR}")
  set(FLITZZIP_ZSTD_RUNTIME_DLL "$<TARGET_FILE:libzstd_shared>")
  set(FLITZZIP_ZSTD_RUNTIME_HASH_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
  set(FLITZZIP_ZSTD_RUNTIME_HASH_HEADER
      "${FLITZZIP_ZSTD_RUNTIME_HASH_DIR}/zstd_runtime_identity.hpp")
  add_custom_command(
    OUTPUT "${FLITZZIP_ZSTD_RUNTIME_HASH_HEADER}"
    COMMAND
      "${CMAKE_COMMAND}" "-DRUNTIME_DLL=${FLITZZIP_ZSTD_RUNTIME_DLL}"
      "-DOUTPUT_HEADER=${FLITZZIP_ZSTD_RUNTIME_HASH_HEADER}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteRuntimeIdentity.cmake"
    DEPENDS libzstd_shared
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteRuntimeIdentity.cmake"
    COMMENT "Embedding the built Zstandard runtime identity"
    VERBATIM)
  add_custom_target(
    flitzzip_zstd_runtime_identity
    DEPENDS "${FLITZZIP_ZSTD_RUNTIME_HASH_HEADER}"
    COMMENT "Preparing Zstandard runtime identity")
  if(NOT EXISTS "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}")
    message(FATAL_ERROR "Bundled wimlib runtime package is missing: "
                        "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}")
  endif()
  file(SHA256 "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}"
       FLITZZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256)
  if(NOT FLITZZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL
     FLITZZIP_WIMLIB_RUNTIME_PACKAGE_SHA256)
    message(FATAL_ERROR "Bundled wimlib runtime package checksum mismatch: "
                        "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}")
  endif()
  if(NOT EXISTS "${FLITZZIP_WIMLIB_RUNTIME_DLL}")
    file(MAKE_DIRECTORY "${FLITZZIP_WIMLIB_RUNTIME_EXTRACT_DIR}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}"
      WORKING_DIRECTORY "${FLITZZIP_WIMLIB_RUNTIME_EXTRACT_DIR}"
      RESULT_VARIABLE FLITZZIP_WIMLIB_RUNTIME_EXTRACT_RESULT)
    if(NOT FLITZZIP_WIMLIB_RUNTIME_EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to extract bundled wimlib runtime package: "
                          "${FLITZZIP_WIMLIB_RUNTIME_PACKAGE}")
    endif()
  endif()
  if(NOT EXISTS "${FLITZZIP_WIMLIB_RUNTIME_DLL}")
    message(
      FATAL_ERROR "Bundled wimlib runtime DLL was not found after extraction: "
                  "${FLITZZIP_WIMLIB_RUNTIME_DLL}")
  endif()
  file(SHA256 "${FLITZZIP_WIMLIB_RUNTIME_DLL}"
       FLITZZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256)
  if(NOT FLITZZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256 STREQUAL
     FLITZZIP_WIMLIB_RUNTIME_DLL_SHA256)
    message(FATAL_ERROR "Bundled wimlib runtime DLL checksum mismatch: "
                        "${FLITZZIP_WIMLIB_RUNTIME_DLL}")
  endif()
endif()

# Purpose: Copy the pinned runtime; target_name names a built executable.
# Outputs: Installs its matching DLL beside the executable after linking.
function(flitzzip_copy_zstd_runtime target_name)
  if(WIN32)
    add_custom_command(
      TARGET "${target_name}"
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${FLITZZIP_ZSTD_RUNTIME_DLL}"
        "$<TARGET_FILE_DIR:${target_name}>/libzstd.dll"
      COMMENT "Copying pinned Zstandard runtime for ${target_name}"
      VERBATIM)
  endif()
endfunction()

# Copy the pinned wimlib runtime DLL beside a built FlitzZip target.
function(flitzzip_copy_wimlib_runtime target_name)
  if(WIN32)
    add_custom_command(
      TARGET "${target_name}"
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${FLITZZIP_WIMLIB_RUNTIME_DLL}"
        "$<TARGET_FILE_DIR:${target_name}>/libwim-15.dll"
      COMMENT "Copying pinned wimlib runtime for ${target_name}"
      VERBATIM)
  endif()
endfunction()

add_library(
  flitzzip_core STATIC
  src/ar/ar_adapter.cpp
  src/arc/arc_adapter.cpp
  src/arj/arj_adapter.cpp
  src/base64/base64_adapter.cpp
  src/bzip2/bzip2_adapter.cpp
  src/bzip2/bzip2_stream.cpp
  src/cab/cab_adapter.cpp
  src/cab/cab_format.cpp
  src/core/archive.cpp
  src/core/decoded_chunk.cpp
  src/core/archive_encode_batch.cpp
  src/core/archive_format.cpp
  src/core/archive_blocks.cpp
  src/core/archive_index.cpp
  src/core/defender_scan.cpp
  src/core/file_manifest.cpp
  src/core/file_publish.cpp
  src/core/windows_directory.cpp
  src/core/integrity.cpp
  src/core/host_memory_budget.cpp
  src/core/path_safety.cpp
  src/core/archive_name_encoding.cpp
  src/core/progress.cpp
  src/core/resource_usage.cpp
  src/core/trusted_runtime.cpp
  src/cpio/cpio_adapter.cpp
  src/gzip/gzip_adapter.cpp
  src/gzip/gzip_stream.cpp
  src/gpu/gpu_codec.cpp
  src/gpu/dictionary_matcher.cpp
  src/gpu/hip_device.cpp
  src/hqx/hqx_adapter.cpp
  src/iso/iso_adapter.cpp
  src/lha/lha_adapter.cpp
  src/lzip/lzip_adapter.cpp
  src/lzip/lzip_stream.cpp
  src/lzma/lzma_adapter.cpp
  src/macbinary/macbinary_adapter.cpp
  src/rpm/rpm_adapter.cpp
  src/rpm/rpm_format.cpp
  src/sevenzip/sevenzip_adapter.cpp
  src/tar/tar_adapter.cpp
  src/unix_compress/unix_compress_adapter.cpp
  src/uue/uue_adapter.cpp
  src/wim/wim_adapter.cpp
  src/xar/xar_adapter.cpp
  src/xxe/xxe_adapter.cpp
  src/xz/xz_adapter.cpp
  src/xz/xz_stream.cpp
  src/zstd/zstd_adapter.cpp
  src/zstd/zstd_runtime.cpp
  src/zstd/zstd_stream.cpp
  src/zip/zip_adapter.cpp)

target_include_directories(
  flitzzip_core
  PUBLIC src third_party/miniz third_party/bzip2 third_party/lzma_sdk/C
         third_party/lhasa/lib/public third_party/wimlib)
target_link_libraries(
  flitzzip_core
  PUBLIC flitzzip_checksum
         miniz
         bzip2
         xz_embedded
         lzma_sdk_7z
         lz4
         lhasa)
target_compile_definitions(
  flitzzip_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                       FLITZZIP_VERSION="${FLITZZIP_PACKAGE_VERSION}")
target_compile_definitions(
  flitzzip_core
  PRIVATE
    FLITZZIP_WIMLIB_RUNTIME_DLL_SHA256="${FLITZZIP_WIMLIB_RUNTIME_DLL_SHA256}")
if(WIN32)
  add_dependencies(flitzzip_core flitzzip_zstd_runtime_identity)
  target_include_directories(
    flitzzip_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
else()
  target_compile_definitions(flitzzip_core
                             PRIVATE FLITZZIP_ZSTD_RUNTIME_DLL_SHA256="")
endif()
target_compile_definitions(flitzzip_core PRIVATE XZ_DEC_CONCATENATED
                                                 XZ_USE_CRC64)

if(MSVC)
  target_compile_options(flitzzip_core PRIVATE /W4 /permissive- /EHsc /wd4201)
else()
  target_compile_options(flitzzip_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FLITZZIP_ENABLE_HIP)
  if(NOT WIN32)
    message(
      FATAL_ERROR "This project currently wires HIP through the Windows ROCm "
                  "toolchain only.")
  endif()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResolveHipArchitecture.cmake")
  flitzzip_resolve_hip_architecture(FLITZZIP_HIP_ARCH "${FLITZZIP_HIP_ARCH}")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/rocm-sdk-lock.json"
       FLITZZIP_ROCM_LOCK)
  string(JSON FLITZZIP_ROCM_REQUIRED_VERSION GET "${FLITZZIP_ROCM_LOCK}"
         version)
  set(FLITZZIP_HIP_PATH
      "$ENV{HIP_PATH}"
      CACHE PATH "Complete pinned ROCm Core SDK installation path")
  set(FLITZZIP_ROCM_MANIFEST
      "${FLITZZIP_HIP_PATH}/share/therock/therock_manifest.json")
  if(NOT EXISTS "${FLITZZIP_ROCM_MANIFEST}")
    message(
      FATAL_ERROR "The complete pinned ROCm Core SDK manifest is required.")
  endif()
  file(READ "${FLITZZIP_ROCM_MANIFEST}" FLITZZIP_ROCM_METADATA LIMIT 1048576)
  string(
    JSON
    FLITZZIP_ROCM_VERSION
    ERROR_VARIABLE
    FLITZZIP_ROCM_VERSION_ERROR
    GET
    "${FLITZZIP_ROCM_METADATA}"
    rocm_version)
  if(FLITZZIP_ROCM_VERSION_ERROR OR NOT FLITZZIP_ROCM_VERSION STREQUAL
                                    FLITZZIP_ROCM_REQUIRED_VERSION)
    message(
      FATAL_ERROR
        "This build requires ROCm Core SDK ${FLITZZIP_ROCM_REQUIRED_VERSION}.")
  endif()
  set(FLITZZIP_HIPCC "${FLITZZIP_HIP_PATH}/bin/hipcc.exe")
  set(FLITZZIP_HIP_LIB "${FLITZZIP_HIP_PATH}/lib/amdhip64.lib")
  if(NOT EXISTS "${FLITZZIP_HIPCC}")
    message(FATAL_ERROR "hipcc.exe not found at ${FLITZZIP_HIPCC}")
  endif()
  if(NOT EXISTS "${FLITZZIP_HIP_LIB}")
    message(FATAL_ERROR "amdhip64.lib not found at ${FLITZZIP_HIP_LIB}")
  endif()
  file(GLOB FLITZZIP_HIP_RUNTIME_DLL_CANDIDATES
       "${FLITZZIP_HIP_PATH}/bin/amdhip64*.dll")
  list(LENGTH FLITZZIP_HIP_RUNTIME_DLL_CANDIDATES
       FLITZZIP_HIP_RUNTIME_DLL_COUNT)
  if(NOT FLITZZIP_HIP_RUNTIME_DLL_COUNT EQUAL 1)
    message(
      FATAL_ERROR
        "Expected exactly one amdhip64 runtime DLL under "
        "${FLITZZIP_HIP_PATH}/bin so the release can record the required "
        "AMD driver runtime major version.")
  endif()
  list(GET FLITZZIP_HIP_RUNTIME_DLL_CANDIDATES 0 FLITZZIP_HIP_RUNTIME_DLL)
  get_filename_component(FLITZZIP_HIP_RUNTIME_DLL_NAME
                         "${FLITZZIP_HIP_RUNTIME_DLL}" NAME)
  file(SHA256 "${FLITZZIP_HIP_RUNTIME_DLL}" FLITZZIP_HIP_RUNTIME_DLL_SHA256)
  file(SIZE "${FLITZZIP_HIP_RUNTIME_DLL}" FLITZZIP_HIP_RUNTIME_DLL_SIZE)

  set(FLITZZIP_HIP_CODEC_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/flitzzip_hip_codec.obj")
  set(FLITZZIP_HIP_STATIC_PREFIX_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/flitzzip_hip_static_prefix.obj")
  set(FLITZZIP_HIP_ADAPTIVE_PREFIX_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/flitzzip_hip_adaptive_prefix.obj")
  set(FLITZZIP_HIP_DICTIONARY_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/flitzzip_hip_dictionary_matcher.obj")
  set(FLITZZIP_HIP_SPARSE_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/flitzzip_hip_sparse_pattern.obj")
  # Purpose: Compile one HIP translation unit with the pinned Windows HIP path.
  # Inputs: source is repo-relative, output is the generated object path, and
  # comment is the build status text. Outputs: Produces an object and a compiler
  # dependency file, tracking the headers consumed by this translation unit.
  function(flitzzip_compile_hip_object source output comment)
    add_custom_command(
      OUTPUT "${output}"
      DEPFILE "${output}.d"
      COMMAND
        powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1" -Source
        "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -Output "${output}" -RepoRoot
        "${CMAKE_CURRENT_SOURCE_DIR}" -Arch "${FLITZZIP_HIP_ARCH}" -HipPath
        "${FLITZZIP_HIP_PATH}" -VcvarsVersion "${FLITZZIP_VCVARS_VERSION}"
        -DependencyFile "${output}.d"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/hip_architecture.ps1"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/rocm_toolchain.ps1"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/process_environment.ps1"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/rocm-sdk-lock.json"
      COMMENT "${comment}"
      VERBATIM)
  endfunction()

  flitzzip_compile_hip_object(
    src/gpu/hip_codec.hip.cpp "${FLITZZIP_HIP_CODEC_OBJECT}"
    "Compiling AMD HIP codec object")
  flitzzip_compile_hip_object(
    src/gpu/hip_codec_static_prefix.hip.cpp
    "${FLITZZIP_HIP_STATIC_PREFIX_OBJECT}"
    "Compiling AMD HIP static-prefix codec object")
  flitzzip_compile_hip_object(
    src/gpu/hip_codec_adaptive_prefix.hip.cpp
    "${FLITZZIP_HIP_ADAPTIVE_PREFIX_OBJECT}"
    "Compiling AMD HIP adaptive-prefix codec object")
  flitzzip_compile_hip_object(
    src/gpu/hip_dictionary_matcher.hip.cpp "${FLITZZIP_HIP_DICTIONARY_OBJECT}"
    "Compiling AMD HIP dictionary codec object")
  flitzzip_compile_hip_object(
    src/gpu/hip_sparse_pattern.hip.cpp "${FLITZZIP_HIP_SPARSE_OBJECT}"
    "Compiling AMD HIP sparse-pattern codec object")
  set(FLITZZIP_HIP_OBJECTS
      "${FLITZZIP_HIP_CODEC_OBJECT}" "${FLITZZIP_HIP_STATIC_PREFIX_OBJECT}"
      "${FLITZZIP_HIP_ADAPTIVE_PREFIX_OBJECT}"
      "${FLITZZIP_HIP_DICTIONARY_OBJECT}" "${FLITZZIP_HIP_SPARSE_OBJECT}")
  add_custom_target(
    flitzzip_hip_object
    DEPENDS ${FLITZZIP_HIP_OBJECTS}
    COMMENT "Preparing AMD HIP codec object")
  # Kernel registration must occur after trusted runtime admission, never in an
  # executable's static constructors. Only a POD dispatch table crosses this
  # DLL.
  add_library(flitzzip_gpu_hip SHARED ${FLITZZIP_HIP_OBJECTS}
                                      src/gpu/hip_kernel_module.cpp)
  add_dependencies(flitzzip_gpu_hip flitzzip_hip_object)
  set_source_files_properties(${FLITZZIP_HIP_OBJECTS}
                              PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  set_target_properties(
    flitzzip_gpu_hip
    PROPERTIES LINKER_LANGUAGE CXX
               OUTPUT_NAME flitzzip_hip_kernels
               MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_include_directories(flitzzip_gpu_hip
                             PUBLIC src "${FLITZZIP_HIP_PATH}/include")
  target_compile_definitions(
    flitzzip_gpu_hip PRIVATE FLITZZIP_ENABLE_HIP=1 __HIP_PLATFORM_AMD__=1
                             NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(flitzzip_gpu_hip PRIVATE "${FLITZZIP_HIP_LIB}")
  target_include_directories(flitzzip_core
                             PUBLIC "${FLITZZIP_HIP_PATH}/include")
  target_compile_definitions(
    flitzzip_core
    PUBLIC __HIP_PLATFORM_AMD__=1
           FLITZZIP_HIP_RUNTIME_DLL_NAME="${FLITZZIP_HIP_RUNTIME_DLL_NAME}"
           FLITZZIP_HIP_KERNEL_DLL_NAME="flitzzip_hip_kernels.dll")
  if(MSVC)
    target_link_libraries(flitzzip_core PUBLIC delayimp)
    target_link_options(flitzzip_core PUBLIC
                        "/DELAYLOAD:${FLITZZIP_HIP_RUNTIME_DLL_NAME}")
  endif()
  target_link_libraries(flitzzip_core PUBLIC "${FLITZZIP_HIP_LIB}")
  target_sources(
    flitzzip_core
    PRIVATE src/gpu/dictionary_candidate.cpp
            src/gpu/sparse_pattern_candidate.cpp
            src/gpu/hip_codec.cpp
            src/gpu/hip_crc.cpp
            src/gpu/hip_codec_static_prefix.cpp
            src/gpu/hip_codec_adaptive_prefix.cpp
            src/gpu/hip_dictionary_matcher.cpp
            src/gpu/hip_sparse_pattern.cpp
            src/gpu/hip_kernel_api.cpp)
  target_compile_definitions(flitzzip_core PUBLIC FLITZZIP_ENABLE_HIP=1)
else()
  target_compile_definitions(flitzzip_core PUBLIC FLITZZIP_ENABLE_HIP=0)
endif()
if(WIN32)
  include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfigureRuntimeDependencies.cmake")
  set(FLITZZIP_RUNTIME_MANIFEST
      "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/flitzzip-runtime-dependencies.json"
  )
  set(FLITZZIP_RUNTIME_BASE_MANIFEST
      "${CMAKE_CURRENT_BINARY_DIR}/runtime-base.json")
  set(FLITZZIP_HIP_GENERATED_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
  flitzzip_configure_runtime_dependencies(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${FLITZZIP_RUNTIME_BASE_MANIFEST}"
    "${FLITZZIP_ENABLE_HIP}")
  set(FLITZZIP_RUNTIME_IDENTITY_ARGS)
  set(FLITZZIP_RUNTIME_IDENTITY_DEPENDS libzstd_shared)
  set(FLITZZIP_RUNTIME_IDENTITY_BYPRODUCTS)
  if(FLITZZIP_ENABLE_HIP)
    set(FLITZZIP_HIP_IDENTITY_HEADER
        "${FLITZZIP_HIP_GENERATED_DIR}/flitzzip_hip_kernel_identity.hpp")
    list(
      APPEND
      FLITZZIP_RUNTIME_IDENTITY_ARGS
      "-DINPUT=$<TARGET_FILE:flitzzip_gpu_hip>"
      "-DHEADER=${FLITZZIP_HIP_IDENTITY_HEADER}"
      "-DPACKAGE_VERSION=${FLITZZIP_PACKAGE_VERSION}")
    list(APPEND FLITZZIP_RUNTIME_IDENTITY_DEPENDS flitzzip_gpu_hip)
    list(APPEND FLITZZIP_RUNTIME_IDENTITY_BYPRODUCTS
         "${FLITZZIP_HIP_IDENTITY_HEADER}")
    target_include_directories(
      flitzzip_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
  endif()
  add_custom_target(
    flitzzip_runtime_identity
    BYPRODUCTS ${FLITZZIP_RUNTIME_IDENTITY_BYPRODUCTS}
    COMMAND
      "${CMAKE_COMMAND}" "-DZSTD_DLL=${FLITZZIP_ZSTD_RUNTIME_DLL}"
      "-DWIM_DLL=${FLITZZIP_WIMLIB_RUNTIME_DLL}"
      "-DBASE_MANIFEST=${FLITZZIP_RUNTIME_BASE_MANIFEST}"
      "-DMANIFEST=${FLITZZIP_RUNTIME_MANIFEST}"
      ${FLITZZIP_RUNTIME_IDENTITY_ARGS} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WritePackagedRuntimeIdentity.cmake"
    DEPENDS
      ${FLITZZIP_RUNTIME_IDENTITY_DEPENDS} "${FLITZZIP_WIMLIB_RUNTIME_DLL}"
      "${FLITZZIP_RUNTIME_BASE_MANIFEST}"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WritePackagedRuntimeIdentity.cmake"
    COMMENT "Binding exact packaged runtime identities"
    VERBATIM)
  add_dependencies(flitzzip_core flitzzip_runtime_identity)
endif()

set(FLITZZIP_LICENSE_NOTICES_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/license-notices.json")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${FLITZZIP_LICENSE_NOTICES_MANIFEST}")
file(READ "${FLITZZIP_LICENSE_NOTICES_MANIFEST}" FLITZZIP_LICENSE_MANIFEST_JSON)
string(JSON FLITZZIP_LICENSE_COUNT LENGTH "${FLITZZIP_LICENSE_MANIFEST_JSON}"
                                          notices)
if(FLITZZIP_LICENSE_COUNT LESS 1)
  message(FATAL_ERROR "License manifest must have nonempty notices")
endif()
math(EXPR FLITZZIP_LICENSE_LAST "${FLITZZIP_LICENSE_COUNT} - 1")
set(FLITZZIP_LICENSE_SOURCES)
foreach(license_index RANGE 0 ${FLITZZIP_LICENSE_LAST} 1)
  string(
    JSON
    license_source
    GET
    "${FLITZZIP_LICENSE_MANIFEST_JSON}"
    notices
    ${license_index}
    source)
  if(NOT license_source MATCHES "^(LICENSE|third_party/[A-Za-z0-9_./-]+)$"
     OR license_source MATCHES "(^|/)\\.\\.(/|$)"
     OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}"
     OR IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}")
    message(FATAL_ERROR "Invalid or missing license source: ${license_source}")
  endif()
  list(APPEND FLITZZIP_LICENSE_SOURCES "${license_source}")
endforeach()
list(REMOVE_DUPLICATES FLITZZIP_LICENSE_SOURCES)
set(FLITZZIP_LICENSE_SOURCE_FILES ${FLITZZIP_LICENSE_SOURCES})
list(TRANSFORM FLITZZIP_LICENSE_SOURCE_FILES
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_executable(flitzzip_cli src/cli/main.cpp src/cli/memory_benchmark.cpp
                            src/cli/memory_benchmark_source.cpp)
target_link_libraries(flitzzip_cli PRIVATE flitzzip_core)
flitzzip_copy_zstd_runtime(flitzzip_cli)
flitzzip_copy_wimlib_runtime(flitzzip_cli)
install(TARGETS flitzzip_cli RUNTIME DESTINATION bin)

if(FLITZZIP_BUILD_GUI AND WIN32)
  set(FLITZZIP_BRAND_LOGO_SVG
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/flitzzip-logo.svg")
  set(FLITZZIP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(FLITZZIP_BRAND_LOGO_HEADER
      "${FLITZZIP_GENERATED_DIR}/flitzzip_brand_logo.hpp")
  set(FLITZZIP_LICENSE_NOTICES_HEADER
      "${FLITZZIP_GENERATED_DIR}/flitzzip_license_notices.hpp")
  add_custom_command(
    OUTPUT "${FLITZZIP_BRAND_LOGO_HEADER}"
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
      -SvgPath "${FLITZZIP_BRAND_LOGO_SVG}" -OutputPath
      "${FLITZZIP_BRAND_LOGO_HEADER}"
    DEPENDS "${FLITZZIP_BRAND_LOGO_SVG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/flitzzip_brand_logo.psm1"
    COMMENT "Generating canonical FlitzZip logo header"
    VERBATIM)
  add_custom_target(
    flitzzip_brand_logo_header
    DEPENDS "${FLITZZIP_BRAND_LOGO_HEADER}"
    COMMENT "Preparing canonical FlitzZip logo header")
  add_custom_command(
    OUTPUT "${FLITZZIP_LICENSE_NOTICES_HEADER}"
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_license_notices_header.ps1"
      -ManifestPath "${FLITZZIP_LICENSE_NOTICES_MANIFEST}" -RepoRoot
      "${CMAKE_CURRENT_SOURCE_DIR}" -OutputPath
      "${FLITZZIP_LICENSE_NOTICES_HEADER}"
    DEPENDS
      "${FLITZZIP_LICENSE_NOTICES_MANIFEST}" ${FLITZZIP_LICENSE_SOURCE_FILES}
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_license_notices_header.ps1"
    COMMENT "Generating FlitzZip license notices header"
    VERBATIM)
  add_custom_target(
    flitzzip_license_notices_header
    DEPENDS "${FLITZZIP_LICENSE_NOTICES_HEADER}"
    COMMENT "Preparing FlitzZip license notices header")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/app/FlitzZip.manifest.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/FlitzZip.manifest" @ONLY)

  add_executable(
    FlitzZip WIN32
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/main_window_archive_support.cpp
    src/app/settings_json.cpp
    src/app/main_window_commands.cpp
    src/app/main_window_controls.cpp
    src/app/main_window_dragdrop.cpp
    src/app/queue_drop_target.cpp
    src/app/main_window_dropdowns.cpp
    src/app/main_window_extract_jobs.cpp
    src/app/main_window_focus.cpp
    src/app/main_window_folder_size.cpp
    src/app/folder_size.cpp
    src/app/main_window_queue_metadata.cpp
    src/app/queue_metadata.cpp
    src/app/main_window_input.cpp
    src/app/main_window_layout.cpp
    src/app/form_layout.cpp
    src/app/main_window_license_dialog.cpp
    src/app/main_window_monitor.cpp
    src/app/main_window_page_clicks.cpp
    src/app/main_window_pages.cpp
    src/app/main_window_queue_interaction.cpp
    src/app/main_window_render_support.cpp
    src/app/main_window_settings_timers.cpp
    src/app/main_window_shell_render.cpp
    src/app/main_window_startup_support.cpp
    src/app/FlitzZip.rc)
  add_dependencies(FlitzZip flitzzip_brand_logo_header
                   flitzzip_license_notices_header)
  target_include_directories(FlitzZip PRIVATE "${FLITZZIP_GENERATED_DIR}")
  target_link_libraries(
    FlitzZip
    PRIVATE flitzzip_core
            comdlg32
            shell32
            shlwapi
            uxtheme
            dwmapi
            ole32
            oleaut32
            RuntimeObject
            gdiplus
            pdh
            psapi)
  target_compile_definitions(FlitzZip PRIVATE UNICODE _UNICODE
                                              _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  flitzzip_copy_zstd_runtime(FlitzZip)
  flitzzip_copy_wimlib_runtime(FlitzZip)
  if(MSVC)
    target_link_options(
      FlitzZip PRIVATE "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/FlitzZip.manifest")
  endif()
  install(TARGETS FlitzZip RUNTIME DESTINATION bin)
endif()

if(WIN32)
  target_link_libraries(
    flitzzip_core
    PUBLIC advapi32
           bcrypt
           ntdll
           cabinet
           ole32
           shell32
           wintrust
           version)
  if(MSVC)
    # One inherited platform contract keeps CLI, GUI, and tests out of Windows
    # version-compatibility shims.
    set(FLITZZIP_WINDOWS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/windows-compatibility.manifest")
    target_link_options(flitzzip_core INTERFACE "/MANIFEST:EMBED"
                        "/MANIFESTINPUT:${FLITZZIP_WINDOWS_MANIFEST}")
    set_property(
      TARGET flitzzip_core
      APPEND
      PROPERTY INTERFACE_LINK_DEPENDS "${FLITZZIP_WINDOWS_MANIFEST}")
  endif()
endif()

if(FLITZZIP_BUILD_TESTS)
  enable_testing()
  if(WIN32)
    # This optional target exercises the same production timing owner without
    # linking HIP or app dependencies.
    add_executable(
      flitzzip_neutron_stage_clock_contracts EXCLUDE_FROM_ALL
      tests/cpp/test_main.cpp tests/cpp/test_neutron_stage_clock.cpp)
    target_include_directories(flitzzip_neutron_stage_clock_contracts
                               PRIVATE src tests/cpp)
    target_compile_features(flitzzip_neutron_stage_clock_contracts
                            PRIVATE cxx_std_20)
    target_compile_definitions(flitzzip_neutron_stage_clock_contracts
                               PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(flitzzip_neutron_stage_clock_contracts PRIVATE /W4
                                                                            /WX)
    endif()
  endif()
  # Compile tests alongside the app targets; only their final executable needs
  # the CLI/GUI prerequisites used by process-level regression cases.
  add_library(
    flitzzip_test_objects OBJECT
    tests/cpp/test_main.cpp
    tests/cpp/test_archive_roundtrip.cpp
    tests/cpp/test_compound_block.cpp
    tests/cpp/test_byte_plane_block.cpp
    tests/cpp/test_archive_verification.cpp
    tests/cpp/test_archive_metadata_failures.cpp
    tests/cpp/test_integrity.cpp
    tests/cpp/test_archive_format.cpp
    tests/cpp/test_adapter_error_contracts.cpp
    tests/cpp/test_ar_compat.cpp
    tests/cpp/test_archive_name_encoding.cpp
    tests/cpp/test_arc_compat.cpp
    tests/cpp/test_arj_compat.cpp
    tests/cpp/test_base64_compat.cpp
    tests/cpp/test_transfer_failures.cpp
    tests/cpp/test_bzip2_compat.cpp
    tests/cpp/test_compression_streams.cpp
    tests/cpp/test_entropy_sampling.cpp
    tests/cpp/test_huffman_decode.cpp
    tests/cpp/test_gzip_headers.cpp
    tests/cpp/test_stream_corruption.cpp
    tests/cpp/test_gpu_telemetry.cpp
    tests/cpp/test_gpu_kernel_boundaries.cpp
    tests/cpp/test_host_memory_budget.cpp
    tests/cpp/test_memory_benchmark_source.cpp
    src/cli/memory_benchmark.cpp
    src/cli/memory_benchmark_source.cpp
    tests/cpp/test_worker_budget.cpp
    tests/cpp/test_windows_compatibility.cpp
    tests/cpp/test_gpu_block_batch.cpp
    tests/cpp/test_cab_compat.cpp
    tests/cpp/test_compat_compression_levels.cpp
    tests/cpp/test_cpio_compat.cpp
    tests/cpp/test_drop_payload.cpp
    tests/cpp/test_queue_drop_target.cpp
    src/app/queue_drop_target.cpp
    tests/cpp/test_file_manifest.cpp
    tests/cpp/test_gzip_compat.cpp
    tests/cpp/test_hqx_compat.cpp
    tests/cpp/test_iso_compat.cpp
    tests/cpp/test_lha_compat.cpp
    tests/cpp/test_lzip_compat.cpp
    tests/cpp/test_lzma_compat.cpp
    tests/cpp/test_macbinary_compat.cpp
    tests/cpp/test_log_retention.cpp
    tests/cpp/test_settings_migration.cpp
    tests/cpp/test_settings_json.cpp
    src/app/settings_json.cpp
    tests/cpp/test_folder_size_cache.cpp
    tests/cpp/test_folder_size.cpp
    src/app/folder_size.cpp
    tests/cpp/test_queue_metadata.cpp
    src/app/queue_metadata.cpp
    tests/cpp/test_gdi_back_buffer.cpp
    tests/cpp/test_system_layout.cpp
    tests/cpp/test_form_layout.cpp
    tests/cpp/test_keyboard_focus.cpp
    tests/cpp/test_window_layout.cpp
    tests/cpp/test_operation_summary.cpp
    tests/cpp/test_operation_destination.cpp
    src/app/form_layout.cpp
    tests/cpp/test_path_safety.cpp
    tests/cpp/test_publication_state.cpp
    tests/cpp/test_rpm_compat.cpp
    tests/cpp/test_resource_usage.cpp
    tests/cpp/test_sdk_byte_access.cpp
    tests/cpp/test_sevenzip_compat.cpp
    tests/cpp/test_sevenzip_ppmd.cpp
    tests/cpp/test_flzip_gpu_prefix.cpp
    tests/cpp/test_dictionary_matcher.cpp
    tests/cpp/test_neutron_dictionary.cpp
    tests/cpp/test_neutron_stage_clock.cpp
    tests/cpp/test_dictionary_block.cpp
    tests/cpp/test_sparse_pattern_block.cpp
    tests/cpp/test_tar_bzip2_compat.cpp
    tests/cpp/test_tar_gzip_compat.cpp
    tests/cpp/test_tar_compat.cpp
    tests/cpp/test_tar_xz_compat.cpp
    tests/cpp/test_tar_zstd_compat.cpp
    tests/cpp/test_unix_compress_compat.cpp
    tests/cpp/test_uue_compat.cpp
    tests/cpp/test_wim_compat.cpp
    tests/cpp/test_xar_compat.cpp
    tests/cpp/test_xxe_compat.cpp
    tests/cpp/test_xz_compat.cpp
    tests/cpp/test_zstd_compat.cpp
    tests/cpp/test_zip_compat.cpp
    tests/cpp/test_zip_verification.cpp)
  target_link_libraries(flitzzip_test_objects PRIVATE flitzzip_core)
  target_compile_definitions(
    flitzzip_test_objects
    PRIVATE
      FLITZZIP_TEST_GUI_BUILT=$<TARGET_EXISTS:FlitzZip>
      FLITZZIP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(flitzzip_tests $<TARGET_OBJECTS:flitzzip_test_objects>)
  target_link_libraries(flitzzip_tests PRIVATE flitzzip_core gdi32 user32
                                               oleaut32 RuntimeObject)
  flitzzip_copy_zstd_runtime(flitzzip_tests)
  flitzzip_copy_wimlib_runtime(flitzzip_tests)
  add_dependencies(flitzzip_tests flitzzip_cli)
  if(TARGET FlitzZip)
    add_dependencies(flitzzip_tests FlitzZip)
  endif()
  add_test(NAME flitzzip_tests COMMAND flitzzip_tests)
  add_test(
    NAME flitzzip_runtime_manifest_contracts
    COMMAND
      "${CMAKE_COMMAND}" "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/runtime-manifest" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_runtime_manifest.cmake")
  set_tests_properties(flitzzip_runtime_manifest_contracts PROPERTIES TIMEOUT
                                                                      15)
  if(WIN32)
    set(FLITZZIP_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    include(cmake/ZstdTests.cmake)
  endif()
endif()

if(FLITZZIP_BUILD_FUZZERS)
  add_executable(flitzzip_archive_index_fuzzer fuzz/archive_index_fuzzer.cpp
                                               src/core/archive_index.cpp)
  target_include_directories(flitzzip_archive_index_fuzzer PRIVATE src)
  target_compile_definitions(flitzzip_archive_index_fuzzer
                             PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_path_safety_fuzzer fuzz/path_safety_fuzzer.cpp
                                             src/core/path_safety.cpp)
  target_include_directories(flitzzip_path_safety_fuzzer PRIVATE src)

  add_executable(flitzzip_cpio_fuzzer fuzz/cpio_fuzzer.cpp)
  target_link_libraries(flitzzip_cpio_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_cpio_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(
    flitzzip_iso_fuzzer
    fuzz/iso_fuzzer.cpp
    src/iso/iso_adapter.cpp
    src/core/file_manifest.cpp
    src/core/file_publish.cpp
    src/core/windows_directory.cpp
    src/core/path_safety.cpp
    src/core/progress.cpp)
  target_include_directories(flitzzip_iso_fuzzer PRIVATE src)
  target_compile_definitions(flitzzip_iso_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)
  if(WIN32)
    target_link_libraries(flitzzip_iso_fuzzer PRIVATE advapi32 bcrypt ntdll)
  endif()

  add_executable(
    flitzzip_cab_header_fuzzer fuzz/cab_header_fuzzer.cpp
                               src/cab/cab_format.cpp src/core/path_safety.cpp)
  target_include_directories(flitzzip_cab_header_fuzzer PRIVATE src)
  target_compile_definitions(flitzzip_cab_header_fuzzer
                             PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_rpm_header_fuzzer fuzz/rpm_header_fuzzer.cpp
                                            src/rpm/rpm_format.cpp)
  target_include_directories(flitzzip_rpm_header_fuzzer PRIVATE src)
  target_compile_definitions(flitzzip_rpm_header_fuzzer
                             PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_sevenzip_fuzzer fuzz/sevenzip_fuzzer.cpp)
  target_link_libraries(flitzzip_sevenzip_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_sevenzip_fuzzer
                             PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_lha_fuzzer fuzz/lha_fuzzer.cpp)
  target_link_libraries(flitzzip_lha_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_lha_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_xar_fuzzer fuzz/xar_fuzzer.cpp)
  target_link_libraries(flitzzip_xar_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_xar_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_lzma_fuzzer fuzz/lzma_fuzzer.cpp)
  target_link_libraries(flitzzip_lzma_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_lzma_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_lzip_fuzzer fuzz/lzip_fuzzer.cpp)
  target_link_libraries(flitzzip_lzip_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_lzip_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_arj_fuzzer fuzz/arj_fuzzer.cpp)
  target_link_libraries(flitzzip_arj_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_arj_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_arc_fuzzer fuzz/arc_fuzzer.cpp)
  target_link_libraries(flitzzip_arc_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_arc_fuzzer PRIVATE FLITZZIP_ENABLE_HIP=0)

  add_executable(flitzzip_macbinary_fuzzer fuzz/macbinary_fuzzer.cpp)
  target_link_libraries(flitzzip_macbinary_fuzzer PRIVATE flitzzip_core)
  target_compile_definitions(flitzzip_macbinary_fuzzer
                             PRIVATE FLITZZIP_ENABLE_HIP=0)

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(
      FATAL_ERROR
        "FLITZZIP_BUILD_FUZZERS requires a compiler with libFuzzer support.")
  endif()
  target_compile_options(flitzzip_archive_index_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_path_safety_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_cpio_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_iso_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_cab_header_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_rpm_header_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_sevenzip_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_lha_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_xar_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_lzma_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_lzip_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_arj_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_arc_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(flitzzip_macbinary_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_archive_index_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_path_safety_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_cpio_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_iso_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_cab_header_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_rpm_header_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_sevenzip_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_lha_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_xar_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_lzma_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_lzip_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_arj_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_arc_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(flitzzip_macbinary_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
endif()

install(FILES README.md LICENSE SECURITY.md CONTRIBUTING.md DESTINATION .)
foreach(license_source IN LISTS FLITZZIP_LICENSE_SOURCES)
  get_filename_component(license_source_dir "${license_source}" DIRECTORY)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}"
          DESTINATION "licenses/${license_source_dir}")
endforeach()
install(FILES resources/licenses/license-notices.json DESTINATION licenses)
if(WIN32)
  install(FILES "${FLITZZIP_ZSTD_RUNTIME_DLL}" DESTINATION bin)
  install(FILES "${FLITZZIP_WIMLIB_RUNTIME_DLL}" DESTINATION bin)
endif()
install(DIRECTORY docs DESTINATION .)
install(DIRECTORY resources/brand resources/benchmarks DESTINATION resources)
if(FLITZZIP_ENABLE_HIP)
  install(TARGETS flitzzip_gpu_hip RUNTIME DESTINATION bin)
endif()
if(WIN32)
  install(FILES "${FLITZZIP_RUNTIME_MANIFEST}" DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "FlitzZip")
set(CPACK_PACKAGE_VENDOR "Efstratios Mitridis")
set(CPACK_PACKAGE_VERSION "${FLITZZIP_PACKAGE_NUMERIC_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${FLITZZIP_PACKAGE_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${FLITZZIP_PACKAGE_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${FLITZZIP_PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Native Windows AMD HIP archive application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "FlitzZip")
set(CPACK_PACKAGE_EXECUTABLES "FlitzZip" "FlitzZip")
set(CPACK_PACKAGE_FILE_NAME "FlitzZip-${FLITZZIP_PACKAGE_VERSION}-win64")
set(CPACK_SYSTEM_NAME "win64")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
               "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "${FLITZZIP_WIX_UPGRADE_GUID}")
set(CPACK_WIX_PRODUCT_GUID "${FLITZZIP_WIX_PRODUCT_GUID}")
set(CPACK_WIX_INSTALL_SCOPE "${FLITZZIP_MSI_INSTALL_SCOPE}")
set(CPACK_WIX_UI_REF "WixUI_FeatureTree")
set(CPACK_WIX_PRODUCT_ICON
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/flitzzip.ico")
set(CPACK_WIX_EXTRA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/flitzzip_desktop_shortcut.wxs")
set(CPACK_WIX_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/flitzzip_wix_patch.xml")
if(FLITZZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
  set(CPACK_WIX_ROOT_FOLDER_ID "ProgramFiles<64>Folder")
endif()
include(cmake/FlitzZipPrecompiledHeaders.cmake)
flitzzip_enable_precompiled_headers()

set(CPACK_GENERATOR "ZIP")
include(CPack)
