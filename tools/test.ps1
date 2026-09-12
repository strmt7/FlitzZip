param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")
& (Join-Path $PSScriptRoot "test_hip_architecture.ps1")
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

$testRunner = Join-Path $build "$Configuration/superzip_tests.exe"
foreach ($arguments in @(
    "__superzip_no_test_must_match_this_filter__",
    "compression_stream_gzip_read_failure_is_terminal compression_stream_zstd_read_failure_is_terminal",
    '"" __superzip_unexpected_second_argument__'
)) {
    $selectionStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $selectionStartInfo.FileName = $testRunner
    $selectionStartInfo.Arguments = $arguments
    $selectionStartInfo.UseShellExecute = $false
    $selectionStartInfo.CreateNoWindow = $true
    $selectionStartInfo.RedirectStandardOutput = $true
    $selectionStartInfo.RedirectStandardError = $true
    # Keep expected failures isolated from the calling shell's native exit status.
    $selectionProcess = [System.Diagnostics.Process]::Start($selectionStartInfo)
    try {
        $selectionOutput = $selectionProcess.StandardOutput.ReadToEndAsync()
        $selectionError = $selectionProcess.StandardError.ReadToEndAsync()
        if (-not $selectionProcess.WaitForExit(30000)) {
            $selectionProcess.Kill()
            throw "The C++ test runner timed out while checking invalid selection arguments."
        }
        $selectionExitCode = $selectionProcess.ExitCode
        $output = $selectionOutput.GetAwaiter().GetResult()
        $errorText = $selectionError.GetAwaiter().GetResult()
    } finally {
        $selectionProcess.Dispose()
    }
    if ($selectionExitCode -ne 2 -or $output -match '\[RUN |\[PASS' -or [string]::IsNullOrWhiteSpace($errorText)) {
        throw "The C++ test runner must reject invalid arguments without running any tests."
    }
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
