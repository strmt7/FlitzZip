param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$CorrectnessOnly
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")
. (Join-Path $PSScriptRoot "cmake_toolchain.ps1")
& (Join-Path $PSScriptRoot "test_hip_architecture.ps1")
& (Join-Path $PSScriptRoot "test_rocm_toolchain.ps1")
& (Join-Path $PSScriptRoot "test_process_environment.ps1")
& (Join-Path $PSScriptRoot "test_cmake_toolchain.ps1")
$ctestExe = Join-Path (Split-Path -Parent (Find-CMake -RepoRoot $repo)) 'ctest.exe'
$build = Join-Path $repo "build"
$testRunner = Join-Path $build "$Configuration/flitzzip_tests.exe"
$cli = Join-Path $build "$Configuration/flitzzip_cli.exe"
if ($CorrectnessOnly.IsPresent) {
    . (Join-Path $PSScriptRoot 'native_test_selection.ps1')
    & py -3 (Join-Path $PSScriptRoot 'native_build_receipt.py') validate --root $repo --configuration $Configuration --require-hip
    if ($LASTEXITCODE -ne 0) { throw 'Correctness-only execution requires current HIP-enabled native binaries.' }
    Assert-FlitzZipNativeSelectionHip -Cli $cli
    $ctestInventory = @(& $ctestExe --test-dir $build -C $Configuration --show-only=json-v1 --stop-on-failure)
    if ($LASTEXITCODE -ne 0) { throw 'Correctness-only CTest inventory failed.' }
    $inventory = ($ctestInventory -join [Environment]::NewLine) | ConvertFrom-Json
    Assert-FlitzZipCTestExecutionInventory -Inventory $inventory
    foreach ($suite in $inventory.tests) {
        $labels = @($suite.properties | Where-Object name -CEQ 'LABELS' | ForEach-Object { $_.value })
        if ($labels -ccontains 'measurement') { Write-Output "DEFERRED CTest measurement suite: $($suite.name)" }
    }
    & $ctestExe --test-dir $build -C $Configuration --interactive-debug-mode 0 --output-on-failure --stop-on-failure -L '^correctness$'
    if ($LASTEXITCODE -ne 0) { throw "Correctness-only CTest failed with exit code $LASTEXITCODE." }
    & $testRunner --correctness-only
    if ($LASTEXITCODE -ne 0) { throw "Correctness-only native registry failed with exit code $LASTEXITCODE." }
    Assert-FlitzZipNativeSelectionHip -Cli $cli
} else {
    & $ctestExe --test-dir $build -C $Configuration --interactive-debug-mode 0 --output-on-failure --stop-on-failure
}
if ($LASTEXITCODE -ne 0) {
    throw "Native tests failed with exit code $LASTEXITCODE."
}

foreach ($arguments in @(
    "__flitzzip_no_test_must_match_this_filter__",
    "compression_stream_gzip_read_failure_is_terminal compression_stream_zstd_read_failure_is_terminal",
    '"" __flitzzip_unexpected_second_argument__'
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

$configuredPackageVersion = Read-FlitzZipCMakeCacheValue -BuildRoot $build -Name "FLITZZIP_PACKAGE_VERSION"
Assert-FlitzZipPackageVersionMatchesBuild -BuildRoot $build -PackageVersion $configuredPackageVersion

$cliVersion = & $cli --version
if ($LASTEXITCODE -ne 0 -or $cliVersion -ne "FlitzZip $configuredPackageVersion") {
    throw "CLI --version must report the configured package version and exit successfully."
}
$cliHelp = & $cli --help
if ($LASTEXITCODE -ne 0 -or -not ($cliHelp -match "flitzzip_cli --version")) {
    throw "CLI --help must advertise --version and exit successfully."
}
if ((Read-FlitzZipCMakeCacheValue -BuildRoot $build -Name 'FLITZZIP_ENABLE_HIP') -eq 'ON') {
    & (Join-Path $PSScriptRoot 'test_hip_kernel_boundary.ps1') -Configuration $Configuration
}
& (Join-Path $PSScriptRoot 'test_memory_benchmark_plan.ps1') -Configuration $Configuration
if ($CorrectnessOnly.IsPresent) {
    Write-Output 'DEFERRED: measured corpus/controller integration; correctness-only execution is not full qualification.'
} else {
    & (Join-Path $PSScriptRoot 'test_memory_benchmark_corpus.ps1') -Configuration $Configuration
}

foreach ($invalidVersion in @("01.0.0", "0.1.0+build.1", "v0.1.0", "latest")) {
    $acceptedInvalidVersion = $false
    try {
        Resolve-FlitzZipPackageVersion -RepoRoot $repo -RequestedVersion $invalidVersion | Out-Null
        $acceptedInvalidVersion = $true
    } catch {
        $acceptedInvalidVersion = $false
    }
    if ($acceptedInvalidVersion) {
        throw "Invalid package version was accepted by tools/version.ps1: $invalidVersion"
    }
}
