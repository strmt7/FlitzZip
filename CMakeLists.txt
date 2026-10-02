cmake_minimum_required(VERSION 3.24)

project(
  SuperZip
  VERSION 0.8.0
  LANGUAGES C CXX)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(SUPERZIP_ENABLE_HIP "Build AMD HIP acceleration backend" ON)
option(SUPERZIP_BUILD_GUI "Build native Windows GUI" ON)
option(SUPERZIP_BUILD_TESTS "Build C++ test binaries" ON)
option(SUPERZIP_BUILD_FUZZERS "Build libFuzzer targets for parser hardening"
       OFF)
set(SUPERZIP_HIP_ARCH
    "gfx1201"
    CACHE STRING "HIP gfx target, comma-separated targets, or release preset")
string(CONCAT SUPERZIP_VCVARS_VERSION_HELP
              "Optional MSVC toolset version used by hipcc on Windows; "
              "empty auto-selects a compatible installed toolset when possible")
set(SUPERZIP_VCVARS_VERSION
    ""
    CACHE STRING "${SUPERZIP_VCVARS_VERSION_HELP}")
set(SUPERZIP_PACKAGE_VERSION
    "${PROJECT_VERSION}"
    CACHE STRING
          "Display version used in package filenames and app diagnostics")
string(
  CONCAT SUPERZIP_PACKAGE_VERSION_REGEX
         "^(0|[1-9][0-9]*)\\."
         "(0|[1-9][0-9]*)\\."
         "(0|[1-9][0-9]*)"
         "(-((0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)"
         "(\\.(0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$")
if(NOT SUPERZIP_PACKAGE_VERSION MATCHES "${SUPERZIP_PACKAGE_VERSION_REGEX}")
  message(
    FATAL_ERROR
      "SUPERZIP_PACKAGE_VERSION must be SemVer without build metadata, "
      "for example 0.1.0 or 0.1.0-beta.1.")
endif()
set(SUPERZIP_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(SUPERZIP_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(SUPERZIP_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
string(
  CONCAT SUPERZIP_PACKAGE_NUMERIC_VERSION "${SUPERZIP_PACKAGE_VERSION_MAJOR}."
         "${SUPERZIP_PACKAGE_VERSION_MINOR}."
         "${SUPERZIP_PACKAGE_VERSION_PATCH}")
string(CONCAT SUPERZIP_MSI_INSTALL_SCOPE_HELP
              "WiX MSI install scope: perMachine installs under Program Files "
              "and requires elevation; perUser installs under LocalAppData")
set(SUPERZIP_MSI_INSTALL_SCOPE
    "perMachine"
    CACHE STRING "${SUPERZIP_MSI_INSTALL_SCOPE_HELP}")
set_property(CACHE SUPERZIP_MSI_INSTALL_SCOPE PROPERTY STRINGS perUser
                                                       perMachine)
if(NOT SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perUser"
   AND NOT SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
  message(
    FATAL_ERROR "SUPERZIP_MSI_INSTALL_SCOPE must be perUser or perMachine.")
endif()
string(
  CONCAT SUPERZIP_MSI_PRODUCT_IDENTITY_HELP
         "Unique MSI package/build identity included in ProductCode "
         "derivation; release workflows set this to the GitHub run identity "
         "so same-version release replacements perform a major upgrade.")
set(SUPERZIP_MSI_PRODUCT_IDENTITY
    ""
    CACHE STRING "${SUPERZIP_MSI_PRODUCT_IDENTITY_HELP}")
if(NOT SUPERZIP_MSI_PRODUCT_IDENTITY)
  string(TIMESTAMP SUPERZIP_MSI_PRODUCT_IDENTITY "local-%Y%m%d%H%M%S" UTC)
endif()
if(NOT SUPERZIP_MSI_PRODUCT_IDENTITY MATCHES "^[A-Za-z0-9_.:-]+$")
  message(
    FATAL_ERROR
      "SUPERZIP_MSI_PRODUCT_IDENTITY must contain only letters, digits, "
      "underscore, dot, colon, or hyphen.")
endif()
set(SUPERZIP_WIX_UPGRADE_GUID "8E0D80F6-859D-4FE9-B082-F0D8048A8B57")
string(
  CONCAT SUPERZIP_WIX_PRODUCT_CODE_SEED "SuperZip MSI ProductCode "
         "${SUPERZIP_PACKAGE_NUMERIC_VERSION} "
         "${SUPERZIP_MSI_INSTALL_SCOPE} " "${SUPERZIP_MSI_PRODUCT_IDENTITY}")
string(SHA256 SUPERZIP_WIX_PRODUCT_CODE_HASH
              "${SUPERZIP_WIX_UPGRADE_GUID}|${SUPERZIP_WIX_PRODUCT_CODE_SEED}")
string(SUBSTRING "${SUPERZIP_WIX_PRODUCT_CODE_HASH}" 0 8
                 SUPERZIP_WIX_PRODUCT_GUID_A)
string(SUBSTRING "${SUPERZIP_WIX_PRODUCT_CODE_HASH}" 8 4
                 SUPERZIP_WIX_PRODUCT_GUID_B)
string(SUBSTRING "${SUPERZIP_WIX_PRODUCT_CODE_HASH}" 12 4
                 SUPERZIP_WIX_PRODUCT_GUID_C)
string(SUBSTRING "${SUPERZIP_WIX_PRODUCT_CODE_HASH}" 16 4
                 SUPERZIP_WIX_PRODUCT_GUID_D)
string(SUBSTRING "${SUPERZIP_WIX_PRODUCT_CODE_HASH}" 20 12
                 SUPERZIP_WIX_PRODUCT_GUID_E)
string(CONCAT SUPERZIP_WIX_PRODUCT_GUID "${SUPERZIP_WIX_PRODUCT_GUID_A}-"
              "${SUPERZIP_WIX_PRODUCT_GUID_B}-${SUPERZIP_WIX_PRODUCT_GUID_C}-"
              "${SUPERZIP_WIX_PRODUCT_GUID_D}-${SUPERZIP_WIX_PRODUCT_GUID_E}")
string(TOUPPER "${SUPERZIP_WIX_PRODUCT_GUID}" SUPERZIP_WIX_PRODUCT_GUID)

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
target_link_libraries(miniz PRIVATE superzip_checksum)
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
  third_party/lzma_sdk/C/Delta.c
  third_party/lzma_sdk/C/Lzma2Dec.c
  third_party/lzma_sdk/C/LzmaDec.c
  third_party/lzma_sdk/C/Ppmd7.c
  third_party/lzma_sdk/C/Ppmd7Dec.c)
target_include_directories(lzma_sdk_7z PUBLIC third_party/lzma_sdk/C)
target_compile_definitions(lzma_sdk_7z PUBLIC Z7_EXTRACT_ONLY Z7_PPMD_SUPPORT)
target_compile_definitions(lzma_sdk_7z PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(lzma_sdk_7z PRIVATE /wd4244 /wd4267 /wd4334)
else()
  target_compile_options(lzma_sdk_7z PRIVATE -Wno-conversion
                                             -Wno-sign-conversion)
endif()

add_library(superzip_checksum STATIC src/core/checksum.cpp)
target_include_directories(superzip_checksum PUBLIC src)
target_link_libraries(superzip_checksum PRIVATE lzma_sdk_7z)
if(MSVC)
  target_compile_options(superzip_checksum PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(superzip_checksum PRIVATE -Wall -Wextra -Wpedantic)
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

set(SUPERZIP_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/upstream")
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE_DIR "${SUPERZIP_UPSTREAM_DIR}/zstd/v1.5.7")
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE_NAME "zstd-v1.5.7.zip")
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE
    "${SUPERZIP_ZSTD_RUNTIME_PACKAGE_DIR}/${SUPERZIP_ZSTD_RUNTIME_PACKAGE_NAME}"
)
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE_SHA256
    "7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4")
set(SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/zstd-v1.5.7-source")
set(SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR
    "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}/zstd-1.5.7")
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE_DIR
    "${SUPERZIP_UPSTREAM_DIR}/wimlib/1.14.5")
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE_NAME "wimlib-1.14.5-windows-x86_64-bin.zip")
string(CONCAT SUPERZIP_WIMLIB_RUNTIME_PACKAGE
              "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE_DIR}/"
              "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE_NAME}")
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE_SHA256
    "2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265")
set(SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256
    "ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495")
set(SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/wimlib-1.14.5-win64-runtime")
set(SUPERZIP_WIMLIB_RUNTIME_DLL
    "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}/libwim-15.dll")
if(WIN32)
  if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
    message(FATAL_ERROR "Bundled Zstandard runtime package is missing: "
                        "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
  endif()
  file(SHA256 "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}"
       SUPERZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256)
  if(NOT SUPERZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL
     SUPERZIP_ZSTD_RUNTIME_PACKAGE_SHA256)
    message(FATAL_ERROR "Bundled Zstandard runtime package checksum mismatch: "
                        "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
  endif()
  set(SUPERZIP_ZSTD_RUNTIME_CMAKE
      "${SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR}/build/cmake/CMakeLists.txt")
  if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_CMAKE}")
    file(MAKE_DIRECTORY "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}"
      WORKING_DIRECTORY "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}"
      RESULT_VARIABLE SUPERZIP_ZSTD_RUNTIME_EXTRACT_RESULT)
    if(NOT SUPERZIP_ZSTD_RUNTIME_EXTRACT_RESULT EQUAL 0)
      message(
        FATAL_ERROR "Failed to extract bundled Zstandard runtime package: "
                    "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
    endif()
  endif()
  if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_CMAKE}")
    message(
      FATAL_ERROR "Pinned Zstandard source was not found after extraction: "
                  "${SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR}")
  endif()
  include(cmake/PatchZstdLegacy.cmake)
  superzip_patch_zstd_legacy("${SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR}")
  # Build the upstream C library without its CLI/test/assembler project. MSVC
  # uses the portable decoder; upstream disables its GNU assembly here.
  set(SUPERZIP_ZSTD_LIBRARY_DIR "${SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR}/lib")
  file(
    GLOB
    SUPERZIP_ZSTD_SOURCES
    CONFIGURE_DEPENDS
    "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/*.c"
    "${SUPERZIP_ZSTD_LIBRARY_DIR}/compress/*.c"
    "${SUPERZIP_ZSTD_LIBRARY_DIR}/decompress/*.c"
    "${SUPERZIP_ZSTD_LIBRARY_DIR}/dictBuilder/*.c"
    "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy/*.c")
  set(SUPERZIP_ZSTD_RESOURCE
      "${SUPERZIP_ZSTD_RUNTIME_SOURCE_DIR}/build/VS2010/libzstd-dll")
  add_library(libzstd_shared SHARED ${SUPERZIP_ZSTD_SOURCES}
                                    "${SUPERZIP_ZSTD_RESOURCE}/libzstd-dll.rc")
  target_include_directories(libzstd_shared
                             PRIVATE "${SUPERZIP_ZSTD_LIBRARY_DIR}")
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
  set(SUPERZIP_ZSTD_RUNTIME_DLL "$<TARGET_FILE:libzstd_shared>")
  set(SUPERZIP_ZSTD_RUNTIME_HASH_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
  set(SUPERZIP_ZSTD_RUNTIME_HASH_HEADER
      "${SUPERZIP_ZSTD_RUNTIME_HASH_DIR}/zstd_runtime_identity.hpp")
  add_custom_command(
    OUTPUT "${SUPERZIP_ZSTD_RUNTIME_HASH_HEADER}"
    COMMAND
      "${CMAKE_COMMAND}" "-DRUNTIME_DLL=${SUPERZIP_ZSTD_RUNTIME_DLL}"
      "-DOUTPUT_HEADER=${SUPERZIP_ZSTD_RUNTIME_HASH_HEADER}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteRuntimeIdentity.cmake"
    DEPENDS libzstd_shared
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteRuntimeIdentity.cmake"
    COMMENT "Embedding the built Zstandard runtime identity"
    VERBATIM)
  add_custom_target(
    superzip_zstd_runtime_identity
    DEPENDS "${SUPERZIP_ZSTD_RUNTIME_HASH_HEADER}"
    COMMENT "Preparing Zstandard runtime identity")
  if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
    message(FATAL_ERROR "Bundled wimlib runtime package is missing: "
                        "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
  endif()
  file(SHA256 "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}"
       SUPERZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256)
  if(NOT SUPERZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL
     SUPERZIP_WIMLIB_RUNTIME_PACKAGE_SHA256)
    message(FATAL_ERROR "Bundled wimlib runtime package checksum mismatch: "
                        "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
  endif()
  if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
    file(MAKE_DIRECTORY "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}"
      WORKING_DIRECTORY "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}"
      RESULT_VARIABLE SUPERZIP_WIMLIB_RUNTIME_EXTRACT_RESULT)
    if(NOT SUPERZIP_WIMLIB_RUNTIME_EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to extract bundled wimlib runtime package: "
                          "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
    endif()
  endif()
  if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
    message(
      FATAL_ERROR "Bundled wimlib runtime DLL was not found after extraction: "
                  "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
  endif()
  file(SHA256 "${SUPERZIP_WIMLIB_RUNTIME_DLL}"
       SUPERZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256)
  if(NOT SUPERZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256 STREQUAL
     SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256)
    message(FATAL_ERROR "Bundled wimlib runtime DLL checksum mismatch: "
                        "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
  endif()
endif()

# Purpose: Copy the pinned runtime; target_name names a built executable.
# Outputs: Installs its matching DLL beside the executable after linking.
function(superzip_copy_zstd_runtime target_name)
  if(WIN32)
    add_custom_command(
      TARGET "${target_name}"
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${SUPERZIP_ZSTD_RUNTIME_DLL}"
        "$<TARGET_FILE_DIR:${target_name}>/libzstd.dll"
      COMMENT "Copying pinned Zstandard runtime for ${target_name}"
      VERBATIM)
  endif()
endfunction()

# Copy the pinned wimlib runtime DLL beside a built SuperZip target.
function(superzip_copy_wimlib_runtime target_name)
  if(WIN32)
    add_custom_command(
      TARGET "${target_name}"
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${SUPERZIP_WIMLIB_RUNTIME_DLL}"
        "$<TARGET_FILE_DIR:${target_name}>/libwim-15.dll"
      COMMENT "Copying pinned wimlib runtime for ${target_name}"
      VERBATIM)
  endif()
endfunction()

add_library(
  superzip_core STATIC
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
  superzip_core
  PUBLIC src third_party/miniz third_party/bzip2 third_party/lzma_sdk/C
         third_party/lhasa/lib/public third_party/wimlib)
target_link_libraries(
  superzip_core
  PUBLIC superzip_checksum
         miniz
         bzip2
         xz_embedded
         lzma_sdk_7z
         lz4
         lhasa)
target_compile_definitions(
  superzip_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                       SUPERZIP_VERSION="${SUPERZIP_PACKAGE_VERSION}")
target_compile_definitions(
  superzip_core
  PRIVATE
    SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256="${SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256}")
if(WIN32)
  add_dependencies(superzip_core superzip_zstd_runtime_identity)
  target_include_directories(
    superzip_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
else()
  target_compile_definitions(superzip_core
                             PRIVATE SUPERZIP_ZSTD_RUNTIME_DLL_SHA256="")
endif()
target_compile_definitions(superzip_core PRIVATE XZ_DEC_CONCATENATED
                                                 XZ_USE_CRC64)

if(MSVC)
  target_compile_options(superzip_core PRIVATE /W4 /permissive- /EHsc /wd4201)
else()
  target_compile_options(superzip_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SUPERZIP_ENABLE_HIP)
  if(NOT WIN32)
    message(
      FATAL_ERROR "This project currently wires HIP through the Windows ROCm "
                  "toolchain only.")
  endif()
  if(NOT DEFINED ENV{HIP_PATH})
    message(
      FATAL_ERROR
        "HIP_PATH is not set. Install AMD ROCm/HIP for Windows or pass "
        "SUPERZIP_ENABLE_HIP=OFF.")
  endif()
  set(SUPERZIP_HIP_PATH
      "$ENV{HIP_PATH}"
      CACHE PATH "AMD ROCm/HIP installation path")
  set(SUPERZIP_HIPCC "${SUPERZIP_HIP_PATH}/bin/hipcc.exe")
  set(SUPERZIP_HIP_LIB "${SUPERZIP_HIP_PATH}/lib/amdhip64.lib")
  if(NOT EXISTS "${SUPERZIP_HIPCC}")
    message(FATAL_ERROR "hipcc.exe not found at ${SUPERZIP_HIPCC}")
  endif()
  if(NOT EXISTS "${SUPERZIP_HIP_LIB}")
    message(FATAL_ERROR "amdhip64.lib not found at ${SUPERZIP_HIP_LIB}")
  endif()
  file(GLOB SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES
       "${SUPERZIP_HIP_PATH}/bin/amdhip64*.dll")
  list(LENGTH SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES
       SUPERZIP_HIP_RUNTIME_DLL_COUNT)
  if(NOT SUPERZIP_HIP_RUNTIME_DLL_COUNT EQUAL 1)
    message(
      FATAL_ERROR
        "Expected exactly one amdhip64 runtime DLL under "
        "${SUPERZIP_HIP_PATH}/bin so the release can record the required "
        "AMD driver runtime major version.")
  endif()
  list(GET SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES 0 SUPERZIP_HIP_RUNTIME_DLL)
  get_filename_component(SUPERZIP_HIP_RUNTIME_DLL_NAME
                         "${SUPERZIP_HIP_RUNTIME_DLL}" NAME)
  file(SHA256 "${SUPERZIP_HIP_RUNTIME_DLL}" SUPERZIP_HIP_RUNTIME_DLL_SHA256)
  file(SIZE "${SUPERZIP_HIP_RUNTIME_DLL}" SUPERZIP_HIP_RUNTIME_DLL_SIZE)

  set(SUPERZIP_HIP_CODEC_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_codec.obj")
  set(SUPERZIP_HIP_STATIC_PREFIX_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_static_prefix.obj")
  set(SUPERZIP_HIP_ADAPTIVE_PREFIX_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_adaptive_prefix.obj")
  set(SUPERZIP_HIP_DICTIONARY_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_dictionary_matcher.obj")
  set(SUPERZIP_HIP_SPARSE_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_sparse_pattern.obj")
  # Purpose: Compile one HIP translation unit with the pinned Windows HIP path.
  # Inputs: source is repo-relative, output is the generated object path, and
  # comment is the build status text. Outputs: Produces an object and a compiler
  # dependency file, tracking the headers consumed by this translation unit.
  function(superzip_compile_hip_object source output comment)
    add_custom_command(
      OUTPUT "${output}"
      DEPFILE "${output}.d"
      COMMAND
        powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1" -Source
        "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -Output "${output}" -RepoRoot
        "${CMAKE_CURRENT_SOURCE_DIR}" -Arch "${SUPERZIP_HIP_ARCH}"
        -VcvarsVersion "${SUPERZIP_VCVARS_VERSION}"
        -DependencyFile "${output}.d"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/hip_architecture.ps1"
      COMMENT "${comment}"
      VERBATIM)
  endfunction()

  superzip_compile_hip_object(
    src/gpu/hip_codec.hip.cpp "${SUPERZIP_HIP_CODEC_OBJECT}"
    "Compiling AMD HIP codec object")
  superzip_compile_hip_object(
    src/gpu/hip_codec_static_prefix.hip.cpp
    "${SUPERZIP_HIP_STATIC_PREFIX_OBJECT}"
    "Compiling AMD HIP static-prefix codec object")
  superzip_compile_hip_object(
    src/gpu/hip_codec_adaptive_prefix.hip.cpp
    "${SUPERZIP_HIP_ADAPTIVE_PREFIX_OBJECT}"
    "Compiling AMD HIP adaptive-prefix codec object")
  superzip_compile_hip_object(
    src/gpu/hip_dictionary_matcher.hip.cpp "${SUPERZIP_HIP_DICTIONARY_OBJECT}"
    "Compiling AMD HIP dictionary codec object")
  superzip_compile_hip_object(
    src/gpu/hip_sparse_pattern.hip.cpp "${SUPERZIP_HIP_SPARSE_OBJECT}"
    "Compiling AMD HIP sparse-pattern codec object")
  set(SUPERZIP_HIP_OBJECTS
      "${SUPERZIP_HIP_CODEC_OBJECT}" "${SUPERZIP_HIP_STATIC_PREFIX_OBJECT}"
      "${SUPERZIP_HIP_ADAPTIVE_PREFIX_OBJECT}"
      "${SUPERZIP_HIP_DICTIONARY_OBJECT}" "${SUPERZIP_HIP_SPARSE_OBJECT}")
  add_custom_target(
    superzip_hip_object
    DEPENDS ${SUPERZIP_HIP_OBJECTS}
    COMMENT "Preparing AMD HIP codec object")
  add_library(superzip_gpu_hip STATIC ${SUPERZIP_HIP_OBJECTS})
  add_dependencies(superzip_gpu_hip superzip_hip_object)
  set_source_files_properties(${SUPERZIP_HIP_OBJECTS}
                              PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  set_target_properties(superzip_gpu_hip PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(superzip_gpu_hip
                             PUBLIC src "${SUPERZIP_HIP_PATH}/include")
  target_compile_definitions(superzip_gpu_hip PUBLIC SUPERZIP_ENABLE_HIP=1)
  target_link_libraries(superzip_gpu_hip PUBLIC "${SUPERZIP_HIP_LIB}")
  target_include_directories(superzip_core
                             PUBLIC "${SUPERZIP_HIP_PATH}/include")
  target_compile_definitions(
    superzip_core
    PUBLIC __HIP_PLATFORM_AMD__=1
           SUPERZIP_HIP_RUNTIME_DLL_NAME="${SUPERZIP_HIP_RUNTIME_DLL_NAME}")
  if(MSVC)
    target_link_libraries(superzip_core PUBLIC delayimp)
    target_link_options(superzip_core PUBLIC
                        "/DELAYLOAD:${SUPERZIP_HIP_RUNTIME_DLL_NAME}")
  endif()
  target_link_libraries(superzip_core PUBLIC superzip_gpu_hip)
  target_sources(superzip_core PRIVATE src/gpu/dictionary_candidate.cpp
                                       src/gpu/sparse_pattern_candidate.cpp)
  target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=1)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip-runtime-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/superzip-runtime-dependencies.json" @ONLY)
else()
  target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=0)
endif()

set(SUPERZIP_LICENSE_NOTICES_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/license-notices.json")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${SUPERZIP_LICENSE_NOTICES_MANIFEST}")
file(READ "${SUPERZIP_LICENSE_NOTICES_MANIFEST}" SUPERZIP_LICENSE_MANIFEST_JSON)
string(JSON SUPERZIP_LICENSE_COUNT LENGTH "${SUPERZIP_LICENSE_MANIFEST_JSON}"
                                          notices)
if(SUPERZIP_LICENSE_COUNT LESS 1)
  message(FATAL_ERROR "License manifest must have nonempty notices")
endif()
math(EXPR SUPERZIP_LICENSE_LAST "${SUPERZIP_LICENSE_COUNT} - 1")
set(SUPERZIP_LICENSE_SOURCES)
foreach(license_index RANGE 0 ${SUPERZIP_LICENSE_LAST} 1)
  string(
    JSON
    license_source
    GET
    "${SUPERZIP_LICENSE_MANIFEST_JSON}"
    notices
    ${license_index}
    source)
  if(NOT license_source MATCHES "^(LICENSE|third_party/[A-Za-z0-9_./-]+)$"
     OR license_source MATCHES "(^|/)\\.\\.(/|$)"
     OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}"
     OR IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}")
    message(FATAL_ERROR "Invalid or missing license source: ${license_source}")
  endif()
  list(APPEND SUPERZIP_LICENSE_SOURCES "${license_source}")
endforeach()
list(REMOVE_DUPLICATES SUPERZIP_LICENSE_SOURCES)
set(SUPERZIP_LICENSE_SOURCE_FILES ${SUPERZIP_LICENSE_SOURCES})
list(TRANSFORM SUPERZIP_LICENSE_SOURCE_FILES
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_executable(superzip_cli src/cli/main.cpp src/cli/memory_benchmark.cpp)
target_link_libraries(superzip_cli PRIVATE superzip_core)
superzip_copy_zstd_runtime(superzip_cli)
superzip_copy_wimlib_runtime(superzip_cli)
install(TARGETS superzip_cli RUNTIME DESTINATION bin)

if(SUPERZIP_BUILD_GUI AND WIN32)
  set(SUPERZIP_BRAND_LOGO_SVG
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/superzip-logo.svg")
  set(SUPERZIP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(SUPERZIP_BRAND_LOGO_HEADER
      "${SUPERZIP_GENERATED_DIR}/superzip_brand_logo.hpp")
  set(SUPERZIP_LICENSE_NOTICES_HEADER
      "${SUPERZIP_GENERATED_DIR}/superzip_license_notices.hpp")
  add_custom_command(
    OUTPUT "${SUPERZIP_BRAND_LOGO_HEADER}"
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
      -SvgPath "${SUPERZIP_BRAND_LOGO_SVG}" -OutputPath
      "${SUPERZIP_BRAND_LOGO_HEADER}"
    DEPENDS "${SUPERZIP_BRAND_LOGO_SVG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/superzip_brand_logo.psm1"
    COMMENT "Generating canonical SuperZip logo header"
    VERBATIM)
  add_custom_target(
    superzip_brand_logo_header
    DEPENDS "${SUPERZIP_BRAND_LOGO_HEADER}"
    COMMENT "Preparing canonical SuperZip logo header")
  add_custom_command(
    OUTPUT "${SUPERZIP_LICENSE_NOTICES_HEADER}"
    COMMAND
      powershell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_license_notices_header.ps1"
      -ManifestPath "${SUPERZIP_LICENSE_NOTICES_MANIFEST}" -RepoRoot
      "${CMAKE_CURRENT_SOURCE_DIR}" -OutputPath
      "${SUPERZIP_LICENSE_NOTICES_HEADER}"
    DEPENDS
      "${SUPERZIP_LICENSE_NOTICES_MANIFEST}" ${SUPERZIP_LICENSE_SOURCE_FILES}
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_license_notices_header.ps1"
    COMMENT "Generating SuperZip license notices header"
    VERBATIM)
  add_custom_target(
    superzip_license_notices_header
    DEPENDS "${SUPERZIP_LICENSE_NOTICES_HEADER}"
    COMMENT "Preparing SuperZip license notices header")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/app/SuperZip.manifest.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/SuperZip.manifest" @ONLY)

  add_executable(
    SuperZip WIN32
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/main_window_archive_support.cpp
    src/app/main_window_commands.cpp
    src/app/main_window_controls.cpp
    src/app/main_window_dragdrop.cpp
    src/app/main_window_dropdowns.cpp
    src/app/main_window_extract_jobs.cpp
    src/app/main_window_focus.cpp
    src/app/main_window_folder_size.cpp
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
    src/app/SuperZip.rc)
  add_dependencies(SuperZip superzip_brand_logo_header
                   superzip_license_notices_header)
  target_include_directories(SuperZip PRIVATE "${SUPERZIP_GENERATED_DIR}")
  target_link_libraries(
    SuperZip
    PRIVATE superzip_core
            comdlg32
            shell32
            shlwapi
            uxtheme
            dwmapi
            ole32
            gdiplus
            pdh
            psapi)
  target_compile_definitions(SuperZip PRIVATE UNICODE _UNICODE
                                              _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  superzip_copy_zstd_runtime(SuperZip)
  superzip_copy_wimlib_runtime(SuperZip)
  if(MSVC)
    target_link_options(
      SuperZip PRIVATE "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/SuperZip.manifest")
  endif()
  install(TARGETS SuperZip RUNTIME DESTINATION bin)
endif()

if(WIN32)
  target_link_libraries(
    superzip_core
    PUBLIC advapi32
           bcrypt
           cabinet
           ole32
           shell32
           wintrust
           version)
  if(MSVC)
    # One inherited platform contract keeps CLI, GUI, and tests out of Windows
    # version-compatibility shims.
    set(SUPERZIP_WINDOWS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/windows-compatibility.manifest")
    target_link_options(superzip_core INTERFACE "/MANIFEST:EMBED"
                        "/MANIFESTINPUT:${SUPERZIP_WINDOWS_MANIFEST}")
    set_property(
      TARGET superzip_core
      APPEND
      PROPERTY INTERFACE_LINK_DEPENDS "${SUPERZIP_WINDOWS_MANIFEST}")
  endif()
endif()

if(SUPERZIP_BUILD_TESTS)
  enable_testing()
  # Compile tests alongside the app targets; only their final executable needs
  # the CLI/GUI prerequisites used by process-level regression cases.
  add_library(
    superzip_test_objects OBJECT
    tests/cpp/test_main.cpp
    tests/cpp/test_archive_roundtrip.cpp
    tests/cpp/test_archive_verification.cpp
    tests/cpp/test_integrity.cpp
    tests/cpp/test_archive_format.cpp
    tests/cpp/test_ar_compat.cpp
    tests/cpp/test_archive_name_encoding.cpp
    tests/cpp/test_arc_compat.cpp
    tests/cpp/test_arj_compat.cpp
    tests/cpp/test_base64_compat.cpp
    tests/cpp/test_bzip2_compat.cpp
    tests/cpp/test_compression_streams.cpp
    tests/cpp/test_gpu_telemetry.cpp
    tests/cpp/test_host_memory_budget.cpp
    tests/cpp/test_worker_budget.cpp
    tests/cpp/test_windows_compatibility.cpp
    tests/cpp/test_gpu_block_batch.cpp
    tests/cpp/test_cab_compat.cpp
    tests/cpp/test_compat_compression_levels.cpp
    tests/cpp/test_cpio_compat.cpp
    tests/cpp/test_drop_payload.cpp
    tests/cpp/test_file_manifest.cpp
    tests/cpp/test_gzip_compat.cpp
    tests/cpp/test_hqx_compat.cpp
    tests/cpp/test_iso_compat.cpp
    tests/cpp/test_lha_compat.cpp
    tests/cpp/test_lzip_compat.cpp
    tests/cpp/test_lzma_compat.cpp
    tests/cpp/test_macbinary_compat.cpp
    tests/cpp/test_log_retention.cpp
    tests/cpp/test_folder_size_cache.cpp
    tests/cpp/test_queue_metadata.cpp
    src/app/queue_metadata.cpp
    tests/cpp/test_gdi_back_buffer.cpp
    tests/cpp/test_system_layout.cpp
    tests/cpp/test_form_layout.cpp
    tests/cpp/test_window_layout.cpp
    tests/cpp/test_operation_summary.cpp
    tests/cpp/test_operation_destination.cpp
    src/app/form_layout.cpp
    tests/cpp/test_path_safety.cpp
    tests/cpp/test_rpm_compat.cpp
    tests/cpp/test_resource_usage.cpp
    tests/cpp/test_sdk_byte_access.cpp
    tests/cpp/test_sevenzip_compat.cpp
    tests/cpp/test_sevenzip_ppmd.cpp
    tests/cpp/test_suzip_gpu_prefix.cpp
    tests/cpp/test_dictionary_matcher.cpp
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
  target_link_libraries(superzip_test_objects PRIVATE superzip_core)
  target_compile_definitions(
    superzip_test_objects
    PRIVATE
      SUPERZIP_TEST_GUI_BUILT=$<TARGET_EXISTS:SuperZip>
      SUPERZIP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(superzip_tests $<TARGET_OBJECTS:superzip_test_objects>)
  target_link_libraries(superzip_tests PRIVATE superzip_core gdi32 user32)
  superzip_copy_zstd_runtime(superzip_tests)
  superzip_copy_wimlib_runtime(superzip_tests)
  add_dependencies(superzip_tests superzip_cli)
  if(TARGET SuperZip)
    add_dependencies(superzip_tests SuperZip)
  endif()
  add_test(NAME superzip_tests COMMAND superzip_tests)
  if(WIN32)
    # Guard-page regressions call the exact DLL built for the product, including
    # dictionary builders that are exported but not used by archive dispatch.
    add_executable(superzip_zstd_bounds_tests tests/cpp/test_main.cpp
                                            tests/cpp/test_zstd_bounds.cpp)
    target_include_directories(superzip_zstd_bounds_tests
                               PRIVATE tests/cpp "${SUPERZIP_ZSTD_LIBRARY_DIR}")
    target_compile_definitions(
      superzip_zstd_bounds_tests PRIVATE ZSTD_DLL_IMPORT=1 NOMINMAX
                                        WIN32_LEAN_AND_MEAN)
    target_link_libraries(superzip_zstd_bounds_tests PRIVATE libzstd_shared)
    superzip_copy_zstd_runtime(superzip_zstd_bounds_tests)
    add_test(NAME superzip_zstd_bounds_tests COMMAND superzip_zstd_bounds_tests)
    set_tests_properties(superzip_zstd_bounds_tests PROPERTIES TIMEOUT 60)
    # Compile the production dependency sources with a deterministic allocator
    # interposer; no alternate decoder implementation is used by these tests.
    add_library(
      superzip_zstd_legacy_fault_objects OBJECT
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy/zstd_v05.c"
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy/zstd_v06.c"
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/legacy/zstd_v07.c")
    target_include_directories(
      superzip_zstd_legacy_fault_objects PRIVATE tests/zstd
                                                 "${SUPERZIP_ZSTD_LIBRARY_DIR}")
    target_compile_definitions(
      superzip_zstd_legacy_fault_objects
      PRIVATE ZSTD_LEGACY_SUPPORT=5 XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM
              _CRT_SECURE_NO_WARNINGS)
    set(_legacy_allocator_header
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/zstd/intercept_allocations.h")
    if(MSVC)
      target_compile_options(superzip_zstd_legacy_fault_objects
                             PRIVATE "/FI${_legacy_allocator_header}")
    else()
      target_compile_options(superzip_zstd_legacy_fault_objects
                             PRIVATE -include "${_legacy_allocator_header}")
    endif()
    add_executable(
      superzip_zstd_legacy_tests
      tests/cpp/test_main.cpp
      tests/cpp/test_zstd_legacy_failures.cpp
      tests/zstd/fault_allocator.c
      tests/zstd/legacy_driver.c
      $<TARGET_OBJECTS:superzip_zstd_legacy_fault_objects>
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/error_private.c"
      "${SUPERZIP_ZSTD_LIBRARY_DIR}/common/xxhash.c")
    target_include_directories(
      superzip_zstd_legacy_tests PRIVATE tests/cpp tests/zstd
                                         "${SUPERZIP_ZSTD_LIBRARY_DIR}")
    target_compile_definitions(
      superzip_zstd_legacy_tests
      PRIVATE ZSTD_LEGACY_SUPPORT=5 XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM
              _CRT_SECURE_NO_WARNINGS)
    add_test(NAME superzip_zstd_legacy_tests COMMAND superzip_zstd_legacy_tests)
    add_test(
      NAME superzip_zstd_legacy_patch
      COMMAND
        "${CMAKE_COMMAND}" "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_zstd_legacy_patch.cmake")
    string(CONCAT _zstd_header_contract
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/"
                  "test_zstd_header_contracts.cmake")
    add_test(
      NAME superzip_zstd_header_contracts
      COMMAND
        "${CMAKE_COMMAND}" "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}" -P
        "${_zstd_header_contract}")
    set_tests_properties(superzip_zstd_legacy_tests superzip_zstd_legacy_patch
                         PROPERTIES TIMEOUT 60)
    set_tests_properties(superzip_zstd_header_contracts PROPERTIES TIMEOUT 150)
  endif()
endif()

if(SUPERZIP_BUILD_FUZZERS)
  add_executable(superzip_archive_index_fuzzer fuzz/archive_index_fuzzer.cpp
                                               src/core/archive_index.cpp)
  target_include_directories(superzip_archive_index_fuzzer PRIVATE src)
  target_compile_definitions(superzip_archive_index_fuzzer
                             PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_path_safety_fuzzer fuzz/path_safety_fuzzer.cpp
                                             src/core/path_safety.cpp)
  target_include_directories(superzip_path_safety_fuzzer PRIVATE src)

  add_executable(superzip_cpio_fuzzer fuzz/cpio_fuzzer.cpp)
  target_link_libraries(superzip_cpio_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_cpio_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(
    superzip_iso_fuzzer
    fuzz/iso_fuzzer.cpp src/iso/iso_adapter.cpp src/core/file_manifest.cpp
    src/core/file_publish.cpp src/core/path_safety.cpp src/core/progress.cpp)
  target_include_directories(superzip_iso_fuzzer PRIVATE src)
  target_compile_definitions(superzip_iso_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(
    superzip_cab_header_fuzzer fuzz/cab_header_fuzzer.cpp
                               src/cab/cab_format.cpp src/core/path_safety.cpp)
  target_include_directories(superzip_cab_header_fuzzer PRIVATE src)
  target_compile_definitions(superzip_cab_header_fuzzer
                             PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_rpm_header_fuzzer fuzz/rpm_header_fuzzer.cpp
                                            src/rpm/rpm_format.cpp)
  target_include_directories(superzip_rpm_header_fuzzer PRIVATE src)
  target_compile_definitions(superzip_rpm_header_fuzzer
                             PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_sevenzip_fuzzer fuzz/sevenzip_fuzzer.cpp)
  target_link_libraries(superzip_sevenzip_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_sevenzip_fuzzer
                             PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_lha_fuzzer fuzz/lha_fuzzer.cpp)
  target_link_libraries(superzip_lha_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_lha_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_xar_fuzzer fuzz/xar_fuzzer.cpp)
  target_link_libraries(superzip_xar_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_xar_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_lzma_fuzzer fuzz/lzma_fuzzer.cpp)
  target_link_libraries(superzip_lzma_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_lzma_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_lzip_fuzzer fuzz/lzip_fuzzer.cpp)
  target_link_libraries(superzip_lzip_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_lzip_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_arj_fuzzer fuzz/arj_fuzzer.cpp)
  target_link_libraries(superzip_arj_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_arj_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_arc_fuzzer fuzz/arc_fuzzer.cpp)
  target_link_libraries(superzip_arc_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_arc_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

  add_executable(superzip_macbinary_fuzzer fuzz/macbinary_fuzzer.cpp)
  target_link_libraries(superzip_macbinary_fuzzer PRIVATE superzip_core)
  target_compile_definitions(superzip_macbinary_fuzzer
                             PRIVATE SUPERZIP_ENABLE_HIP=0)

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(
      FATAL_ERROR
        "SUPERZIP_BUILD_FUZZERS requires a compiler with libFuzzer support.")
  endif()
  target_compile_options(superzip_archive_index_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_path_safety_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_cpio_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_iso_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_cab_header_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_rpm_header_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_sevenzip_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_lha_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_xar_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_lzma_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_lzip_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_arj_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_arc_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_compile_options(superzip_macbinary_fuzzer
                         PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_archive_index_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_path_safety_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_cpio_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_iso_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_cab_header_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_rpm_header_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_sevenzip_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_lha_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_xar_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_lzma_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_lzip_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_arj_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_arc_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
  target_link_options(superzip_macbinary_fuzzer PRIVATE
                      -fsanitize=fuzzer,address,undefined)
endif()

install(FILES README.md LICENSE SECURITY.md CONTRIBUTING.md DESTINATION .)
foreach(license_source IN LISTS SUPERZIP_LICENSE_SOURCES)
  get_filename_component(license_source_dir "${license_source}" DIRECTORY)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${license_source}"
          DESTINATION "licenses/${license_source_dir}")
endforeach()
install(FILES resources/licenses/license-notices.json DESTINATION licenses)
if(WIN32)
  install(FILES "${SUPERZIP_ZSTD_RUNTIME_DLL}" DESTINATION bin)
  install(FILES "${SUPERZIP_WIMLIB_RUNTIME_DLL}" DESTINATION bin)
endif()
install(DIRECTORY docs DESTINATION .)
if(SUPERZIP_ENABLE_HIP)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/superzip-runtime-dependencies.json"
          DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "SuperZip")
set(CPACK_PACKAGE_VENDOR "Efstratios Mitridis")
set(CPACK_PACKAGE_VERSION "${SUPERZIP_PACKAGE_NUMERIC_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${SUPERZIP_PACKAGE_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${SUPERZIP_PACKAGE_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${SUPERZIP_PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Native Windows AMD HIP archive application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SuperZip")
set(CPACK_PACKAGE_EXECUTABLES "SuperZip" "SuperZip")
set(CPACK_PACKAGE_FILE_NAME "SuperZip-${SUPERZIP_PACKAGE_VERSION}-win64")
set(CPACK_SYSTEM_NAME "win64")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
               "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "${SUPERZIP_WIX_UPGRADE_GUID}")
set(CPACK_WIX_PRODUCT_GUID "${SUPERZIP_WIX_PRODUCT_GUID}")
set(CPACK_WIX_INSTALL_SCOPE "${SUPERZIP_MSI_INSTALL_SCOPE}")
set(CPACK_WIX_UI_REF "WixUI_FeatureTree")
set(CPACK_WIX_PRODUCT_ICON
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/superzip.ico")
set(CPACK_WIX_EXTRA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip_desktop_shortcut.wxs")
set(CPACK_WIX_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip_wix_patch.xml")
if(SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
  set(CPACK_WIX_ROOT_FOLDER_ID "ProgramFiles<64>Folder")
endif()
set(CPACK_GENERATOR "ZIP")
include(CPack)
