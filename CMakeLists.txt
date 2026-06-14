cmake_minimum_required(VERSION 3.24)

project(SuperZip VERSION 0.1.0 LANGUAGES C CXX)

option(SUPERZIP_ENABLE_HIP "Build AMD HIP acceleration backend" OFF)
option(SUPERZIP_BUILD_GUI "Build native Windows GUI" ON)
option(SUPERZIP_BUILD_TESTS "Build C++ test binaries" ON)
set(SUPERZIP_HIP_ARCH "gfx1201" CACHE STRING "AMD GPU target architecture for HIP kernels")
set(SUPERZIP_VCVARS_VERSION "14.44" CACHE STRING "MSVC toolset version used by hipcc on Windows")
set(SUPERZIP_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Display version used in package filenames and app diagnostics")

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

add_library(superzip_core STATIC
    src/core/archive.cpp
    src/core/archive_index.cpp
    src/core/checksum.cpp
    src/core/defender_scan.cpp
    src/core/file_manifest.cpp
    src/core/integrity.cpp
    src/core/path_safety.cpp
    src/core/progress.cpp
    src/gpu/gpu_codec_cpu.cpp
    src/gpu/hip_device.cpp
    src/zip/zip_adapter.cpp
)

target_include_directories(superzip_core PUBLIC src third_party/miniz)
target_link_libraries(superzip_core PUBLIC miniz)
target_compile_definitions(superzip_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    SUPERZIP_VERSION="${SUPERZIP_PACKAGE_VERSION}"
)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
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
    target_compile_definitions(superzip_core PUBLIC __HIP_PLATFORM_AMD__=1)
    target_link_libraries(superzip_core PUBLIC superzip_gpu_hip)
    target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=1)
else()
    target_compile_definitions(superzip_core PUBLIC SUPERZIP_ENABLE_HIP=0)
endif()

add_executable(superzip_cli src/cli/main.cpp)
target_link_libraries(superzip_cli PRIVATE superzip_core)
install(TARGETS superzip_cli RUNTIME DESTINATION bin)

if(SUPERZIP_BUILD_GUI AND WIN32)
    add_executable(SuperZip WIN32
        src/app/main.cpp
        src/app/main_window.cpp
    )
    target_link_libraries(SuperZip PRIVATE superzip_core comdlg32 shell32 shlwapi uxtheme dwmapi ole32)
    target_compile_definitions(SuperZip PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    if(MSVC)
        target_link_options(SuperZip PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/SuperZip.manifest")
    endif()
    install(TARGETS SuperZip RUNTIME DESTINATION bin)
endif()

if(WIN32)
    target_link_libraries(superzip_core PUBLIC bcrypt)
endif()

if(SUPERZIP_BUILD_TESTS)
    enable_testing()
    add_executable(superzip_tests
        tests/cpp/test_main.cpp
        tests/cpp/test_archive_roundtrip.cpp
        tests/cpp/test_integrity.cpp
        tests/cpp/test_path_safety.cpp
        tests/cpp/test_zip_compat.cpp
    )
    target_link_libraries(superzip_tests PRIVATE superzip_core)
    add_test(NAME superzip_tests COMMAND superzip_tests)
endif()

install(FILES
    README.md
    LICENSE
    SECURITY.md
    CONTRIBUTING.md
    DESTINATION .
)
install(DIRECTORY docs DESTINATION .)

set(CPACK_PACKAGE_NAME "SuperZip")
set(CPACK_PACKAGE_VENDOR "strmt7")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows AMD HIP archive application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SuperZip")
set(CPACK_PACKAGE_EXECUTABLES "SuperZip" "SuperZip")
set(CPACK_CREATE_DESKTOP_LINKS "SuperZip")
set(CPACK_PACKAGE_FILE_NAME "SuperZip-${SUPERZIP_PACKAGE_VERSION}-win64")
set(CPACK_SYSTEM_NAME "win64")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "8E0D80F6-859D-4FE9-B082-F0D8048A8B57")
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
set(CPACK_GENERATOR "ZIP")
include(CPack)
