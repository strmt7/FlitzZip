cmake_minimum_required(VERSION 3.24)

project(SuperZip VERSION 0.1.0 LANGUAGES C CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(SUPERZIP_ENABLE_HIP "Build AMD HIP acceleration backend" ON)
option(SUPERZIP_BUILD_GUI "Build native Windows GUI" ON)
option(SUPERZIP_BUILD_TESTS "Build C++ test binaries" ON)
option(SUPERZIP_BUILD_FUZZERS "Build libFuzzer targets for parser hardening" OFF)
set(SUPERZIP_HIP_ARCH "gfx1201" CACHE STRING "AMD GPU target architecture for HIP kernels")
set(SUPERZIP_VCVARS_VERSION "" CACHE STRING "Optional MSVC toolset version used by hipcc on Windows; empty auto-selects a compatible installed toolset when possible")
set(SUPERZIP_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Display version used in package filenames and app diagnostics")
if(NOT SUPERZIP_PACKAGE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)(\\.(0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$")
    message(FATAL_ERROR "SUPERZIP_PACKAGE_VERSION must be SemVer without build metadata, for example 0.1.0 or 0.1.0-beta.1.")
endif()
set(SUPERZIP_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(SUPERZIP_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(SUPERZIP_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
set(SUPERZIP_PACKAGE_NUMERIC_VERSION "${SUPERZIP_PACKAGE_VERSION_MAJOR}.${SUPERZIP_PACKAGE_VERSION_MINOR}.${SUPERZIP_PACKAGE_VERSION_PATCH}")
set(SUPERZIP_MSI_INSTALL_SCOPE "perMachine" CACHE STRING "WiX MSI install scope: perMachine installs under Program Files and requires elevation; perUser installs under LocalAppData")
set_property(CACHE SUPERZIP_MSI_INSTALL_SCOPE PROPERTY STRINGS perUser perMachine)
if(NOT SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perUser" AND NOT SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
    message(FATAL_ERROR "SUPERZIP_MSI_INSTALL_SCOPE must be perUser or perMachine.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(bzip2 STATIC
    third_party/bzip2/blocksort.c
    third_party/bzip2/bz_internal_error.c
    third_party/bzip2/bzlib.c
    third_party/bzip2/compress.c
    third_party/bzip2/crctable.c
    third_party/bzip2/decompress.c
    third_party/bzip2/huffman.c
    third_party/bzip2/randtable.c
)
target_include_directories(bzip2 PUBLIC third_party/bzip2)
target_compile_definitions(bzip2 PUBLIC BZ_NO_STDIO)
target_compile_definitions(bzip2 PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(xz_embedded STATIC
    third_party/xz_embedded/linux/lib/xz/xz_crc32.c
    third_party/xz_embedded/linux/lib/xz/xz_crc64.c
    third_party/xz_embedded/linux/lib/xz/xz_dec_lzma2.c
    third_party/xz_embedded/linux/lib/xz/xz_dec_stream.c
)
target_include_directories(xz_embedded
    PUBLIC
        third_party/xz_embedded/linux/include/linux
        third_party/xz_embedded/userspace
    PRIVATE
        third_party/xz_embedded/linux/lib/xz
)
target_compile_definitions(xz_embedded PRIVATE XZ_DEC_DYNALLOC _CRT_SECURE_NO_WARNINGS)

add_library(lzma_sdk_7z STATIC
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
    third_party/lzma_sdk/C/Ppmd7Dec.c
)
target_include_directories(lzma_sdk_7z PUBLIC third_party/lzma_sdk/C)
target_compile_definitions(lzma_sdk_7z PUBLIC Z7_EXTRACT_ONLY Z7_PPMD_SUPPORT)
target_compile_definitions(lzma_sdk_7z PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(lzma_sdk_7z PRIVATE /wd4244 /wd4267 /wd4334)
else()
    target_compile_options(lzma_sdk_7z PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()

add_library(lhasa STATIC
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
    third_party/lhasa/lib/pm2_decoder.c
)
target_include_directories(lhasa
    PUBLIC
        third_party/lhasa/lib/public
    PRIVATE
        third_party/lhasa/lib
)
target_compile_definitions(lhasa PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(lhasa PRIVATE /wd4244 /wd4267)
else()
    target_compile_options(lhasa PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()

set(SUPERZIP_ZSTD_RUNTIME_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/upstream/zstd/v1.5.7/zstd-v1.5.7-win64.zip")
set(SUPERZIP_ZSTD_RUNTIME_PACKAGE_SHA256 "acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9")
set(SUPERZIP_ZSTD_RUNTIME_DLL_SHA256 "8f07e1112ed283e5cd2798833e9a3c32d8961381bc36da04af57a1b0ca9bd40b")
set(SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/zstd-v1.5.7-win64-runtime")
set(SUPERZIP_ZSTD_RUNTIME_DLL "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}/zstd-v1.5.7-win64/dll/libzstd.dll")
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/upstream/wimlib/1.14.5/wimlib-1.14.5-windows-x86_64-bin.zip")
set(SUPERZIP_WIMLIB_RUNTIME_PACKAGE_SHA256 "2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265")
set(SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256 "ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495")
set(SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/wimlib-1.14.5-win64-runtime")
set(SUPERZIP_WIMLIB_RUNTIME_DLL "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}/libwim-15.dll")
if(WIN32)
    if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
        message(FATAL_ERROR "Bundled Zstandard runtime package is missing: ${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
    endif()
    file(SHA256 "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}" SUPERZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256)
    if(NOT SUPERZIP_ZSTD_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL SUPERZIP_ZSTD_RUNTIME_PACKAGE_SHA256)
        message(FATAL_ERROR "Bundled Zstandard runtime package checksum mismatch: ${SUPERZIP_ZSTD_RUNTIME_PACKAGE}")
    endif()
    if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_DLL}")
        file(MAKE_DIRECTORY "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${SUPERZIP_ZSTD_RUNTIME_PACKAGE}" DESTINATION "${SUPERZIP_ZSTD_RUNTIME_EXTRACT_DIR}")
    endif()
    if(NOT EXISTS "${SUPERZIP_ZSTD_RUNTIME_DLL}")
        message(FATAL_ERROR "Bundled Zstandard runtime DLL was not found after extraction: ${SUPERZIP_ZSTD_RUNTIME_DLL}")
    endif()
    file(SHA256 "${SUPERZIP_ZSTD_RUNTIME_DLL}" SUPERZIP_ZSTD_RUNTIME_DLL_ACTUAL_SHA256)
    if(NOT SUPERZIP_ZSTD_RUNTIME_DLL_ACTUAL_SHA256 STREQUAL SUPERZIP_ZSTD_RUNTIME_DLL_SHA256)
        message(FATAL_ERROR "Bundled Zstandard runtime DLL checksum mismatch: ${SUPERZIP_ZSTD_RUNTIME_DLL}")
    endif()
    if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
        message(FATAL_ERROR "Bundled wimlib runtime package is missing: ${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
    endif()
    file(SHA256 "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}" SUPERZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256)
    if(NOT SUPERZIP_WIMLIB_RUNTIME_PACKAGE_ACTUAL_SHA256 STREQUAL SUPERZIP_WIMLIB_RUNTIME_PACKAGE_SHA256)
        message(FATAL_ERROR "Bundled wimlib runtime package checksum mismatch: ${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}")
    endif()
    if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
        file(MAKE_DIRECTORY "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${SUPERZIP_WIMLIB_RUNTIME_PACKAGE}" DESTINATION "${SUPERZIP_WIMLIB_RUNTIME_EXTRACT_DIR}")
    endif()
    if(NOT EXISTS "${SUPERZIP_WIMLIB_RUNTIME_DLL}")
        message(FATAL_ERROR "Bundled wimlib runtime DLL was not found after extraction: ${SUPERZIP_WIMLIB_RUNTIME_DLL}")
    endif()
    file(SHA256 "${SUPERZIP_WIMLIB_RUNTIME_DLL}" SUPERZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256)
    if(NOT SUPERZIP_WIMLIB_RUNTIME_DLL_ACTUAL_SHA256 STREQUAL SUPERZIP_WIMLIB_RUNTIME_DLL_SHA256)
        message(FATAL_ERROR "Bundled wimlib runtime DLL checksum mismatch: ${SUPERZIP_WIMLIB_RUNTIME_DLL}")
    endif()
endif()

function(superzip_copy_zstd_runtime target_name)
    if(WIN32)
        add_custom_command(
            TARGET "${target_name}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SUPERZIP_ZSTD_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/libzstd.dll"
            VERBATIM
        )
    endif()
endfunction()

function(superzip_copy_wimlib_runtime target_name)
    if(WIN32)
        add_custom_command(
            TARGET "${target_name}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SUPERZIP_WIMLIB_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/libwim-15.dll"
            VERBATIM
        )
    endif()
endfunction()

add_library(superzip_core STATIC
    src/ar/ar_adapter.cpp
    src/arc/arc_adapter.cpp
    src/arj/arj_adapter.cpp
    src/base64/base64_adapter.cpp
    src/bzip2/bzip2_adapter.cpp
    src/bzip2/bzip2_stream.cpp
    src/cab/cab_adapter.cpp
    src/cab/cab_format.cpp
    src/core/archive.cpp
    src/core/archive_format.cpp
    src/core/archive_blocks.cpp
    src/core/archive_index.cpp
    src/core/checksum.cpp
    src/core/defender_scan.cpp
    src/core/file_manifest.cpp
    src/core/file_publish.cpp
    src/core/integrity.cpp
    src/core/path_safety.cpp
    src/core/progress.cpp
    src/cpio/cpio_adapter.cpp
    src/gzip/gzip_adapter.cpp
    src/gzip/gzip_stream.cpp
    src/gpu/gpu_codec.cpp
    src/gpu/hip_device.cpp
    src/iso/iso_adapter.cpp
    src/lha/lha_adapter.cpp
    src/lzip/lzip_adapter.cpp
    src/lzip/lzip_stream.cpp
    src/lzma/lzma_adapter.cpp
    src/rpm/rpm_adapter.cpp
    src/rpm/rpm_format.cpp
    src/sevenzip/sevenzip_adapter.cpp
    src/tar/tar_adapter.cpp
    src/unix_compress/unix_compress_adapter.cpp
    src/uue/uue_adapter.cpp
    src/wim/wim_adapter.cpp
    src/xar/xar_adapter.cpp
    src/xz/xz_adapter.cpp
    src/xz/xz_stream.cpp
    src/zstd/zstd_adapter.cpp
    src/zstd/zstd_runtime.cpp
    src/zstd/zstd_stream.cpp
    src/zip/zip_adapter.cpp
)

target_include_directories(superzip_core PUBLIC src third_party/miniz third_party/bzip2 third_party/lzma_sdk/C third_party/lhasa/lib/public third_party/wimlib)
target_link_libraries(superzip_core PUBLIC miniz bzip2 xz_embedded lzma_sdk_7z lhasa)
target_compile_definitions(superzip_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    SUPERZIP_VERSION="${SUPERZIP_PACKAGE_VERSION}"
)
target_compile_definitions(superzip_core PRIVATE XZ_DEC_CONCATENATED XZ_USE_CRC64)

if(MSVC)
    target_compile_options(superzip_core PRIVATE /W4 /permissive- /EHsc /wd4201)
else()
    target_compile_options(superzip_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SUPERZIP_ENABLE_HIP)
    if(NOT WIN32)
        message(FATAL_ERROR "This project currently wires HIP through the Windows ROCm toolchain only.")
    endif()
    if(NOT DEFINED ENV{HIP_PATH})
        message(FATAL_ERROR "HIP_PATH is not set. Install AMD ROCm/HIP for Windows or pass SUPERZIP_ENABLE_HIP=OFF.")
    endif()
    set(SUPERZIP_HIP_PATH "$ENV{HIP_PATH}" CACHE PATH "AMD ROCm/HIP installation path")
    set(SUPERZIP_HIPCC "${SUPERZIP_HIP_PATH}/bin/hipcc.exe")
    set(SUPERZIP_HIP_LIB "${SUPERZIP_HIP_PATH}/lib/amdhip64.lib")
    if(NOT EXISTS "${SUPERZIP_HIPCC}")
        message(FATAL_ERROR "hipcc.exe not found at ${SUPERZIP_HIPCC}")
    endif()
    if(NOT EXISTS "${SUPERZIP_HIP_LIB}")
        message(FATAL_ERROR "amdhip64.lib not found at ${SUPERZIP_HIP_LIB}")
    endif()
    file(GLOB SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES "${SUPERZIP_HIP_PATH}/bin/amdhip64*.dll")
    list(LENGTH SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES SUPERZIP_HIP_RUNTIME_DLL_COUNT)
    if(NOT SUPERZIP_HIP_RUNTIME_DLL_COUNT EQUAL 1)
        message(FATAL_ERROR "Expected exactly one amdhip64 runtime DLL under ${SUPERZIP_HIP_PATH}/bin so the release can record the required AMD driver runtime major version.")
    endif()
    list(GET SUPERZIP_HIP_RUNTIME_DLL_CANDIDATES 0 SUPERZIP_HIP_RUNTIME_DLL)
    get_filename_component(SUPERZIP_HIP_RUNTIME_DLL_NAME "${SUPERZIP_HIP_RUNTIME_DLL}" NAME)
    file(SHA256 "${SUPERZIP_HIP_RUNTIME_DLL}" SUPERZIP_HIP_RUNTIME_DLL_SHA256)
    file(SIZE "${SUPERZIP_HIP_RUNTIME_DLL}" SUPERZIP_HIP_RUNTIME_DLL_SIZE)

    set(SUPERZIP_HIP_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/superzip_hip_codec.obj")
    add_custom_command(
        OUTPUT "${SUPERZIP_HIP_OBJECT}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1"
            -Source "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/hip_codec.hip.cpp"
            -Output "${SUPERZIP_HIP_OBJECT}"
            -RepoRoot "${CMAKE_CURRENT_SOURCE_DIR}"
            -Arch "${SUPERZIP_HIP_ARCH}"
            -VcvarsVersion "${SUPERZIP_VCVARS_VERSION}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/hip_codec.hip.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/core/archive_block_types.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/core/archive_blocks.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/gpu_codec.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_hip_object.ps1"
        VERBATIM
    )
    add_custom_target(superzip_hip_object DEPENDS "${SUPERZIP_HIP_OBJECT}")
    add_library(superzip_gpu_hip STATIC "${SUPERZIP_HIP_OBJECT}")
    add_dependencies(superzip_gpu_hip superzip_hip_object)
    set_source_files_properties("${SUPERZIP_HIP_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    set_target_properties(superzip_gpu_hip PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(superzip_gpu_hip PUBLIC src "${SUPERZIP_HIP_PATH}/include")
    target_compile_definitions(superzip_gpu_hip PUBLIC SUPERZIP_ENABLE_HIP=1)
    target_link_libraries(superzip_gpu_hip PUBLIC "${SUPERZIP_HIP_LIB}")
    target_include_directories(superzip_core PUBLIC "${SUPERZIP_HIP_PATH}/include")
    target_compile_definitions(superzip_core PUBLIC
        __HIP_PLATFORM_AMD__=1
        SUPERZIP_HIP_RUNTIME_DLL_NAME="${SUPERZIP_HIP_RUNTIME_DLL_NAME}"
    )
    if(MSVC)
        target_link_libraries(superzip_core PUBLIC delayimp)
        target_link_options(superzip_core PUBLIC "/DELAYLOAD:${SUPERZIP_HIP_RUNTIME_DLL_NAME}")
    endif()
    target_link_libraries(superzip_core PUBLIC superzip_gpu_hip)
    target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=1)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip-runtime-dependencies.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/superzip-runtime-dependencies.json"
        @ONLY
    )
else()
    target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=0)
endif()

add_executable(superzip_cli src/cli/main.cpp)
target_link_libraries(superzip_cli PRIVATE superzip_core)
superzip_copy_zstd_runtime(superzip_cli)
superzip_copy_wimlib_runtime(superzip_cli)
install(TARGETS superzip_cli RUNTIME DESTINATION bin)

if(SUPERZIP_BUILD_GUI AND WIN32)
    set(SUPERZIP_BRAND_LOGO_SVG "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/superzip-logo.svg")
    set(SUPERZIP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(SUPERZIP_BRAND_LOGO_HEADER "${SUPERZIP_GENERATED_DIR}/superzip_brand_logo.hpp")
    add_custom_command(
        OUTPUT "${SUPERZIP_BRAND_LOGO_HEADER}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
            -SvgPath "${SUPERZIP_BRAND_LOGO_SVG}"
            -OutputPath "${SUPERZIP_BRAND_LOGO_HEADER}"
        DEPENDS
            "${SUPERZIP_BRAND_LOGO_SVG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_brand_logo_header.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/superzip_brand_logo.psm1"
        VERBATIM
    )
    add_custom_target(superzip_brand_logo_header DEPENDS "${SUPERZIP_BRAND_LOGO_HEADER}")

    add_executable(SuperZip WIN32
        src/app/main.cpp
        src/app/main_window.cpp
        src/app/SuperZip.rc
    )
    add_dependencies(SuperZip superzip_brand_logo_header)
    target_include_directories(SuperZip PRIVATE "${SUPERZIP_GENERATED_DIR}")
    target_link_libraries(SuperZip PRIVATE superzip_core comdlg32 shell32 shlwapi uxtheme dwmapi ole32 gdiplus pdh psapi)
    target_compile_definitions(SuperZip PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    superzip_copy_zstd_runtime(SuperZip)
    superzip_copy_wimlib_runtime(SuperZip)
    if(MSVC)
        target_link_options(SuperZip PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/SuperZip.manifest")
    endif()
    install(TARGETS SuperZip RUNTIME DESTINATION bin)
endif()

if(WIN32)
    target_link_libraries(superzip_core PUBLIC bcrypt cabinet)
endif()

if(SUPERZIP_BUILD_TESTS)
    enable_testing()
    add_executable(superzip_tests
        tests/cpp/test_main.cpp
        tests/cpp/test_archive_roundtrip.cpp
        tests/cpp/test_integrity.cpp
        tests/cpp/test_archive_format.cpp
        tests/cpp/test_ar_compat.cpp
        tests/cpp/test_arc_compat.cpp
        tests/cpp/test_arj_compat.cpp
        tests/cpp/test_base64_compat.cpp
        tests/cpp/test_bzip2_compat.cpp
        tests/cpp/test_cab_compat.cpp
        tests/cpp/test_cpio_compat.cpp
        tests/cpp/test_gzip_compat.cpp
        tests/cpp/test_iso_compat.cpp
        tests/cpp/test_lha_compat.cpp
        tests/cpp/test_lzip_compat.cpp
        tests/cpp/test_lzma_compat.cpp
        tests/cpp/test_path_safety.cpp
        tests/cpp/test_rpm_compat.cpp
        tests/cpp/test_sevenzip_compat.cpp
        tests/cpp/test_tar_bzip2_compat.cpp
        tests/cpp/test_tar_gzip_compat.cpp
        tests/cpp/test_tar_compat.cpp
        tests/cpp/test_tar_xz_compat.cpp
        tests/cpp/test_tar_zstd_compat.cpp
        tests/cpp/test_unix_compress_compat.cpp
        tests/cpp/test_uue_compat.cpp
        tests/cpp/test_wim_compat.cpp
        tests/cpp/test_xar_compat.cpp
        tests/cpp/test_xz_compat.cpp
        tests/cpp/test_zstd_compat.cpp
        tests/cpp/test_zip_compat.cpp
    )
    target_link_libraries(superzip_tests PRIVATE superzip_core)
    superzip_copy_zstd_runtime(superzip_tests)
    superzip_copy_wimlib_runtime(superzip_tests)
    target_compile_definitions(superzip_tests PRIVATE SUPERZIP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME superzip_tests COMMAND superzip_tests)
endif()

if(SUPERZIP_BUILD_FUZZERS)
    add_executable(superzip_archive_index_fuzzer
        fuzz/archive_index_fuzzer.cpp
        src/core/archive_index.cpp
    )
    target_include_directories(superzip_archive_index_fuzzer PRIVATE src)
    target_compile_definitions(superzip_archive_index_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_path_safety_fuzzer
        fuzz/path_safety_fuzzer.cpp
        src/core/path_safety.cpp
    )
    target_include_directories(superzip_path_safety_fuzzer PRIVATE src)

    add_executable(superzip_cpio_fuzzer
        fuzz/cpio_fuzzer.cpp
    )
    target_link_libraries(superzip_cpio_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_cpio_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_iso_fuzzer
        fuzz/iso_fuzzer.cpp
        src/iso/iso_adapter.cpp
        src/core/file_publish.cpp
        src/core/path_safety.cpp
        src/core/progress.cpp
    )
    target_include_directories(superzip_iso_fuzzer PRIVATE src)
    target_compile_definitions(superzip_iso_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_cab_header_fuzzer
        fuzz/cab_header_fuzzer.cpp
        src/cab/cab_format.cpp
        src/core/path_safety.cpp
    )
    target_include_directories(superzip_cab_header_fuzzer PRIVATE src)
    target_compile_definitions(superzip_cab_header_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_rpm_header_fuzzer
        fuzz/rpm_header_fuzzer.cpp
        src/rpm/rpm_format.cpp
    )
    target_include_directories(superzip_rpm_header_fuzzer PRIVATE src)
    target_compile_definitions(superzip_rpm_header_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_sevenzip_fuzzer
        fuzz/sevenzip_fuzzer.cpp
    )
    target_link_libraries(superzip_sevenzip_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_sevenzip_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_lha_fuzzer
        fuzz/lha_fuzzer.cpp
    )
    target_link_libraries(superzip_lha_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_lha_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_xar_fuzzer
        fuzz/xar_fuzzer.cpp
    )
    target_link_libraries(superzip_xar_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_xar_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_lzma_fuzzer
        fuzz/lzma_fuzzer.cpp
    )
    target_link_libraries(superzip_lzma_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_lzma_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_lzip_fuzzer
        fuzz/lzip_fuzzer.cpp
    )
    target_link_libraries(superzip_lzip_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_lzip_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_arj_fuzzer
        fuzz/arj_fuzzer.cpp
    )
    target_link_libraries(superzip_arj_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_arj_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    add_executable(superzip_arc_fuzzer
        fuzz/arc_fuzzer.cpp
    )
    target_link_libraries(superzip_arc_fuzzer PRIVATE superzip_core)
    target_compile_definitions(superzip_arc_fuzzer PRIVATE SUPERZIP_ENABLE_HIP=0)

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SUPERZIP_BUILD_FUZZERS requires a compiler with libFuzzer support.")
    endif()
    target_compile_options(superzip_archive_index_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_path_safety_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_cpio_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_iso_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_cab_header_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_rpm_header_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_sevenzip_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_lha_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_xar_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_lzma_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_lzip_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_arj_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(superzip_arc_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_archive_index_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_path_safety_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_cpio_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_iso_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_cab_header_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_rpm_header_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_sevenzip_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_lha_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_xar_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_lzma_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_lzip_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_arj_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(superzip_arc_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(FILES
    README.md
    LICENSE
    SECURITY.md
    CONTRIBUTING.md
    DESTINATION .
)
if(WIN32)
    install(FILES "${SUPERZIP_ZSTD_RUNTIME_DLL}" DESTINATION bin)
    install(FILES "${SUPERZIP_WIMLIB_RUNTIME_DLL}" DESTINATION bin)
endif()
install(DIRECTORY docs DESTINATION .)
if(SUPERZIP_ENABLE_HIP)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/superzip-runtime-dependencies.json" DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "SuperZip")
set(CPACK_PACKAGE_VENDOR "SuperZip Technologies")
set(CPACK_PACKAGE_VERSION "${SUPERZIP_PACKAGE_NUMERIC_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${SUPERZIP_PACKAGE_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${SUPERZIP_PACKAGE_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${SUPERZIP_PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows AMD HIP archive application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SuperZip")
set(CPACK_PACKAGE_EXECUTABLES "SuperZip" "SuperZip")
set(CPACK_PACKAGE_FILE_NAME "SuperZip-${SUPERZIP_PACKAGE_VERSION}-win64")
set(CPACK_SYSTEM_NAME "win64")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "8E0D80F6-859D-4FE9-B082-F0D8048A8B57")
set(CPACK_WIX_INSTALL_SCOPE "${SUPERZIP_MSI_INSTALL_SCOPE}")
set(CPACK_WIX_UI_REF "WixUI_FeatureTree")
set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip_desktop_shortcut.wxs")
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/superzip_wix_patch.xml")
if(SUPERZIP_MSI_INSTALL_SCOPE STREQUAL "perMachine")
    set(CPACK_WIX_ROOT_FOLDER_ID "ProgramFiles<64>Folder")
endif()
set(CPACK_GENERATOR "ZIP")
include(CPack)
