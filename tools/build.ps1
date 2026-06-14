param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$EnableHip,
    [switch]$ConfigureOnly,
    [string]$HipArch = "gfx1201",
    [string]$VcvarsVersion = "14.44"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"

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
$hipArg = if ($EnableHip.IsPresent) { "ON" } else { "OFF" }
$configureArgs = @(
    "-S", $repo,
    "-B", $build,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DSUPERZIP_ENABLE_HIP=$hipArg",
    "-DSUPERZIP_HIP_ARCH=$HipArch",
    "-DSUPERZIP_VCVARS_VERSION=$VcvarsVersion",
    "-DSUPERZIP_BUILD_GUI=ON",
    "-DSUPERZIP_BUILD_TESTS=ON"
)
& $cmake @configureArgs

if (-not $ConfigureOnly) {
    & $cmake --build $build --config $Configuration --parallel
}
