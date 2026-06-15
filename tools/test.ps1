param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
$cmakeCTest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if (Test-Path $cmakeCTest) {
    $ctestExe = $cmakeCTest
} elseif ($ctest) {
    $ctestExe = $ctest.Source
} else {
    throw "ctest was not found."
}
$build = Join-Path $repo "build"
& $ctestExe --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "ctest failed with exit code $LASTEXITCODE."
}

$configuredPackageVersion = Read-SuperZipCMakeCacheValue -BuildRoot $build -Name "SUPERZIP_PACKAGE_VERSION"
Assert-SuperZipPackageVersionMatchesBuild -BuildRoot $build -PackageVersion $configuredPackageVersion

foreach ($invalidVersion in @("01.0.0", "0.1.0+build.1", "v0.1.0", "latest")) {
    $acceptedInvalidVersion = $false
    try {
        Resolve-SuperZipPackageVersion -RepoRoot $repo -RequestedVersion $invalidVersion | Out-Null
        $acceptedInvalidVersion = $true
    } catch {
        $acceptedInvalidVersion = $false
    }
    if ($acceptedInvalidVersion) {
        throw "Invalid package version was accepted by tools/version.ps1: $invalidVersion"
    }
}
