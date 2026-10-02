# Purpose: Exercise the production configure resolver without an SDK or GPU.
# Inputs: REPO_ROOT, SELECTION and EXPECTED name an exact resolution case.
# Outputs: Fails on policy divergence or invalid input; compiles no device.
if(NOT DEFINED REPO_ROOT
   OR NOT DEFINED SELECTION
   OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "REPO_ROOT, SELECTION and EXPECTED are required")
endif()
include("${REPO_ROOT}/cmake/ResolveHipArchitecture.cmake")
superzip_resolve_hip_architecture(actual "${SELECTION}")
if(NOT actual STREQUAL EXPECTED)
  message(FATAL_ERROR "Configure-time HIP targets differ from expected policy")
endif()
