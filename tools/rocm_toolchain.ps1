# Purpose: Read the shared whole-distribution lock used by local builds and hosted provisioning.
# Inputs: None; reads the repository-owned JSON beside this helper.
# Outputs: Returns pinned distribution metadata; propagates malformed or unavailable input errors.
function Get-RocmSdkLock {
    return [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'rocm-sdk-lock.json')) | ConvertFrom-Json
}

# Purpose: Validate the complete pinned ROCm Core SDK without substituting bundled components.
# Inputs: Root is an explicit SDK directory; files are inspected but never executed or modified.
# Outputs: Returns its absolute root; throws for missing files, unsafe paths or a different distribution version.
function Assert-RocmSdkRoot {
    param([Parameter(Mandatory = $true)][string]$Root)
    if ($Root -match '[\r\n"&|<>^%!]' -or -not [IO.Path]::IsPathRooted($Root)) {
        throw 'ROCm SDK root must be an absolute path without command-shell metacharacters.'
    }
    $full = [IO.Path]::GetFullPath($Root)
    $lock = Get-RocmSdkLock
    $manifest = Join-Path $full 'share/therock/therock_manifest.json'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf) -or
        (Get-Item -LiteralPath $manifest).Length -gt 1048576) {
        throw "The complete ROCm Core SDK $($lock.version) distribution manifest is required."
    }
    $metadata = [IO.File]::ReadAllText($manifest) | ConvertFrom-Json
    if ($metadata.rocm_version -cne $lock.version) {
        throw "The repository toolchain requires the pinned ROCm Core SDK $($lock.version) distribution."
    }
    foreach ($relative in @('bin/hipcc.exe', 'lib/amdhip64.lib', 'include/hip/hip_version.h',
            'lib/llvm/bin/clang.exe', 'lib/llvm/amdgcn/bitcode/ocml.bc')) {
        $file = Join-Path $full $relative
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "The complete ROCm SDK is missing a required build input: $relative"
        }
    }
    return $full
}

# Purpose: Select an explicitly requested SDK or the isolated pinned repository toolchain.
# Inputs: RepoRoot identifies the checkout; RequestedPath overrides discovery; HIP_PATH is a final external candidate.
# Outputs: Returns a validated ROCm 10 root without changing host installations or environment settings.
function Resolve-RocmSdkRoot {
    param([Parameter(Mandatory = $true)][string]$RepoRoot, [AllowEmptyString()][string]$RequestedPath = '')
    if ($RequestedPath) { return Assert-RocmSdkRoot -Root $RequestedPath }
    $lock = Get-RocmSdkLock
    $cache = Join-Path $RepoRoot "out/rocm-core-sdk-$($lock.version)/sdk"
    if (Test-Path -LiteralPath $cache -PathType Container) { return Assert-RocmSdkRoot -Root $cache }
    if ($env:HIP_PATH) { return Assert-RocmSdkRoot -Root $env:HIP_PATH }
    throw "Provision the complete ROCm Core SDK $($lock.version) with tools/bootstrap_rocm_sdk.py; see docs/rocm-toolchain.md."
}

# Purpose: Scope AMD compiler discovery to one build action and restore every caller setting.
# Inputs: Root is a validated SDK root; Action invokes compilers or their dependency scans, never the product runtime.
# Outputs: Forwards action output/errors and restores process environment even after an exception.
function Invoke-RocmCompilerEnvironment {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][scriptblock]$Action)
    $settings = @{
        HIP_PATH = $Root; ROCM_PATH = $Root; HIP_PLATFORM = 'amd'
        HIP_DEVICE_LIB_PATH = (Join-Path $Root 'lib/llvm/amdgcn/bitcode')
        LLVM_PATH = (Join-Path $Root 'lib/llvm')
    }
    $previous = @{}
    foreach ($name in $settings.Keys) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    try {
        foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process') }
        & $Action
    } finally {
        foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
