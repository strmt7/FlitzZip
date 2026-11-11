# Purpose: Exercise the production configure resolver without an SDK or GPU.
# Inputs: REPO_ROOT, SELECTION and EXPECTED name an exact resolution case.
# FAILURE_CASE and FIXTURE_ROOT instead select an isolated subprocess failure.
# Outputs: Fails on policy divergence or invalid input; compiles no device.
if(DEFINED FAILURE_CASE)
  if(NOT DEFINED REPO_ROOT OR NOT DEFINED FIXTURE_ROOT)
    message(FATAL_ERROR "Failure cases require REPO_ROOT and FIXTURE_ROOT")
  endif()
  set(_resolver_root "${FIXTURE_ROOT}/${FAILURE_CASE}")
  file(MAKE_DIRECTORY "${_resolver_root}/cmake" "${_resolver_root}/tools")
  configure_file(
    "${REPO_ROOT}/cmake/ResolveHipArchitecture.cmake"
    "${_resolver_root}/cmake/ResolveHipArchitecture.cmake" COPYONLY)
  if(FAILURE_CASE STREQUAL "stdout-exit")
    set(_body "[Console]::Out.WriteLine('resolution-stdout-sentinel'); exit 7")
  elseif(FAILURE_CASE STREQUAL "stderr-exit")
    set(_body "[Console]::Error.WriteLine('resolution-stderr-sentinel');exit 9")
  elseif(FAILURE_CASE STREQUAL "malformed-success")
    set(_body "[Console]::Out.WriteLine('gfx1201 trailing'); exit 0")
  elseif(FAILURE_CASE STREQUAL "deadline")
    # Valid partial output must not admit a process that never completed.
    string(CONCAT _body "[Console]::Out.WriteLine('gfx1201'); "
                       "[Threading.Thread]::Sleep(30000)")
  elseif(FAILURE_CASE STREQUAL "missing-command")
    set(_body "[Console]::Out.WriteLine('gfx1201'); exit 0")
  else()
    message(FATAL_ERROR "Unknown resolver failure case")
  endif()
  file(WRITE "${_resolver_root}/tools/hip_architecture.ps1"
       "param([string]$Architecture)\n${_body}\n")
  set(REPO_ROOT "${_resolver_root}")
  set(SELECTION "gfx1201")
  set(EXPECTED "gfx1201")
endif()
if(NOT DEFINED REPO_ROOT
   OR NOT DEFINED SELECTION
   OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "REPO_ROOT, SELECTION and EXPECTED are required")
endif()
include("${REPO_ROOT}/cmake/ResolveHipArchitecture.cmake")
flitzzip_resolve_hip_architecture(actual "${SELECTION}")
if(NOT actual STREQUAL EXPECTED)
  message(FATAL_ERROR "Configure-time HIP targets differ from expected policy")
endif()
