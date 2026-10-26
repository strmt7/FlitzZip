# CPack loads this after selecting its generator. Portable developer builds may
# use SemVer prerelease suffixes; Windows Installer cannot order those suffixes.
if(CPACK_GENERATOR STREQUAL "WIX")
  if(NOT CPACK_FLITZZIP_PACKAGE_VERSION MATCHES
     "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(
      FATAL_ERROR
        "MSI requires three numeric fields; use the beta release track "
        "without a version suffix.")
  endif()
  if(CMAKE_MATCH_1 GREATER 255
     OR CMAKE_MATCH_2 GREATER 255
     OR CMAKE_MATCH_3 GREATER 65535)
    message(FATAL_ERROR "MSI version exceeds 255.255.65535; "
                        "truncation and wrapping are prohibited.")
  endif()
endif()
