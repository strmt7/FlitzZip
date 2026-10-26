cmake_minimum_required(VERSION 4.4)
project(
  FlitzZipInstallerProtocolFixture
  VERSION ${FIXTURE_VERSION}
  LANGUAGES NONE)

# This package is installation-protocol evidence, never an executable product.
# Reuse production authoring and identity lineage rather than duplicating its
# MajorUpgrade template, feature identifiers or shortcut components.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/FlitzZip.exe"
     "protocol fixture ${FIXTURE_ID}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/FlitzZip.exe" DESTINATION bin)
if(FIXTURE_OBSOLETE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obsolete-owned.txt"
       "old package owned this file\n")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/obsolete-owned.txt"
          DESTINATION bin)
endif()
set(CPACK_PACKAGE_NAME "FlitzZip")
set(CPACK_PACKAGE_VENDOR "Efstratios Mitridis")
set(CPACK_PACKAGE_VERSION "${FIXTURE_VERSION}")
set(CPACK_FLITZZIP_PACKAGE_VERSION "${FIXTURE_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "fixture-${FIXTURE_ID}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "FlitzZip")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "8E0D80F6-859D-4FE9-B082-F0D8048A8B57")
set(CPACK_WIX_PRODUCT_GUID "${FIXTURE_PRODUCT_CODE}")
set(CPACK_WIX_INSTALL_SCOPE "${FIXTURE_SCOPE}")
if(FIXTURE_SCOPE STREQUAL "perMachine")
  set(CPACK_WIX_ROOT_FOLDER_ID "ProgramFiles<64>Folder")
endif()
set(CPACK_WIX_UI_REF "WixUI_FeatureTree")
set(CPACK_WIX_EXTRA_SOURCES
    "${FLITZZIP_ROOT}/cmake/flitzzip_desktop_shortcut.wxs")
set(CPACK_WIX_PATCH_FILE "${FLITZZIP_ROOT}/cmake/flitzzip_wix_patch.xml")
if(FIXTURE_FAILURE)
  enable_language(C)
  add_library(fixture_failure SHARED fail_upgrade.c)
  file(
    GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fail-$<CONFIG>.wxs"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/fail_upgrade.wxs.in")
  list(APPEND CPACK_WIX_EXTRA_SOURCES
       "${CMAKE_CURRENT_BINARY_DIR}/fail-Release.wxs")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${FLITZZIP_ROOT}/cmake/FlitzZipMsiVersion.cmake")
set(CPACK_GENERATOR "WIX")
include(CPack)
