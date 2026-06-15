param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$EnableHip,
    [switch]$CpuOnlyValidation,
    [switch]$ConfigureOnly,
    [string]$HipArch = "gfx1201",
    [string]$VcvarsVersion = "",
    [string]$PackageVersion = "",
    [ValidateSet("perUser", "perMachine")] [string]$MsiInstallScope = "perMachine"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
. (Join-Path $PSScriptRoot "version.ps1")

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

# Purpose: Invoke a native executable and promote non-zero process exits to PowerShell failures.
# Inputs: FilePath is the executable; Arguments is the argv array; Operation is the diagnostic label.
# Outputs: Returns no value; throws when the native executable reports failure.
function Invoke-NativeTool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$Operation
    )

    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Operation failed with exit code $exitCode."
    }
}

$cmake = Find-CMake
if ($EnableHip.IsPresent -and $CpuOnlyValidation.IsPresent) {
    throw "-EnableHip and -CpuOnlyValidation are mutually exclusive."
}
$hipArg = if ($CpuOnlyValidation.IsPresent) { "OFF" } else { "ON" }
$PackageVersion = Resolve-SuperZipPackageVersion -RepoRoot $repo -RequestedVersion $PackageVersion
$configureArgs = @(
    "-S", $repo,
    "-B", $build,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DSUPERZIP_ENABLE_HIP=$hipArg",
    "-DSUPERZIP_HIP_ARCH=$HipArch",
    "-DSUPERZIP_VCVARS_VERSION=$VcvarsVersion",
    "-DSUPERZIP_PACKAGE_VERSION=$PackageVersion",
    "-DSUPERZIP_MSI_INSTALL_SCOPE=$MsiInstallScope",
    "-DSUPERZIP_BUILD_GUI=ON",
    "-DSUPERZIP_BUILD_TESTS=ON"
)
Invoke-NativeTool -FilePath $cmake -Arguments $configureArgs -Operation "CMake configure"

if (-not $ConfigureOnly) {
    $buildArgs = @("--build", $build, "--config", $Configuration, "--parallel")
    Invoke-NativeTool -FilePath $cmake -Arguments $buildArgs -Operation "CMake build"
}
