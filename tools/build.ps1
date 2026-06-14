param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$EnableHip,
    [switch]$CpuOnlyValidation,
    [switch]$ConfigureOnly,
    [string]$HipArch = "gfx1201",
    [string]$VcvarsVersion = "14.44",
    [string]$PackageVersion = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"

# Purpose: Resolve the package display version from an explicit argument or CMake project metadata.
# Inputs: RequestedVersion is an optional SemVer string supplied by the caller.
# Outputs: Returns the version string used for CMake package diagnostics and filenames.
function Resolve-PackageVersion {
    param([string]$RequestedVersion)
    if ($RequestedVersion) {
        return $RequestedVersion
    }
    $cmakeLists = Join-Path $repo "CMakeLists.txt"
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $match = [regex]::Match($text, "project\s*\(\s*SuperZip\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", "IgnoreCase")
    if (-not $match.Success) {
        throw "Unable to resolve package version from CMakeLists.txt."
    }
    return $match.Groups[1].Value
}

# Purpose: Find a usable CMake executable on a Windows development or CI host.
# Inputs: None; probes known install paths and PATH.
# Outputs: Returns the CMake executable path or throws when CMake is unavailable.
function Find-CMake {
    $candidates = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "CMake was not found."
}

$cmake = Find-CMake
if ($EnableHip.IsPresent -and $CpuOnlyValidation.IsPresent) {
    throw "-EnableHip and -CpuOnlyValidation are mutually exclusive."
}
$hipArg = if ($CpuOnlyValidation.IsPresent) { "OFF" } else { "ON" }
$PackageVersion = Resolve-PackageVersion -RequestedVersion $PackageVersion
$configureArgs = @(
    "-S", $repo,
    "-B", $build,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DSUPERZIP_ENABLE_HIP=$hipArg",
    "-DSUPERZIP_HIP_ARCH=$HipArch",
    "-DSUPERZIP_VCVARS_VERSION=$VcvarsVersion",
    "-DSUPERZIP_PACKAGE_VERSION=$PackageVersion",
    "-DSUPERZIP_BUILD_GUI=ON",
    "-DSUPERZIP_BUILD_TESTS=ON"
)
& $cmake @configureArgs

if (-not $ConfigureOnly) {
    & $cmake --build $build --config $Configuration --parallel
}
