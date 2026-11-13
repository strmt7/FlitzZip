$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
. (Join-Path $PSScriptRoot 'build_parallelism.ps1')
$root = Split-Path -Parent $PSScriptRoot
$ownedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/native-runner-contracts'))
$fixtureRoot = Join-Path $ownedRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

# Capture one owned fixture process, draining both pipes before disposal. The
# enclosing bounded verifier owns its process tree; the local deadline rejects
# a stuck fixture. Expected nonzero exits are returned for independent assertions.
function Invoke-NativeContractProcess {
    param([string]$Runner, [string]$Arguments)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Runner
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            $process.WaitForExit(5000) | Out-Null
            throw 'Owned native runner fixture exceeded its deadline.'
        }
        $text = $output.GetAwaiter().GetResult()
        $errorText = $errors.GetAwaiter().GetResult()
        return [pscustomobject]@{ exit_code = $process.ExitCode; stdout = $text; stderr = $errorText }
    } finally { $process.Dispose() }
}

function Assert-NativeRunnerCase {
    param([string]$Runner, [string]$Arguments, [int]$Exit, [string[]]$Names)
    $result = Invoke-NativeContractProcess -Runner $Runner -Arguments $Arguments
    $actual = @([regex]::Matches($result.stdout, '(?m)^\[RUN \] (?<name>\w+)\r?$') |
            ForEach-Object { $_.Groups['name'].Value })
        if ($result.exit_code -ne $Exit -or
            (($actual | Sort-Object) -join '|') -ne (($Names | Sort-Object) -join '|') -or
            ($Exit -eq 2 -and [string]::IsNullOrWhiteSpace($result.stderr))) {
            throw "Native runner contract failed for '$Arguments': exit=$($result.exit_code), cases=$($actual -join ','); $($result.stdout) $($result.stderr)"
        }
}

# Run the unchanged real driver in an owned filesystem with recording children.
# The fixtures cannot load product binaries or execute a benchmark. Their journal
# independently checks command retention, role separation and failure propagation.
function New-NativeDriverFixture {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([string]$Root, [string]$Probe)
    if (-not $PSCmdlet.ShouldProcess($Root, 'Create isolated native-driver fixture')) { return }
    $tools = Join-Path $Root 'tools'
    $bin = Join-Path $Root 'build/Release'
    New-Item -ItemType Directory -Path $tools, $bin -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test.ps1') -Destination $tools
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native_test_selection.ps1') -Destination $tools
    foreach ($name in @('ctest.exe', 'flitzzip_tests.exe', 'flitzzip_cli.exe')) {
        Copy-Item -LiteralPath $Probe -Destination (Join-Path $bin $name)
    }
    foreach ($name in @('test_hip_architecture', 'test_rocm_toolchain', 'test_process_environment',
            'test_cmake_toolchain', 'test_hip_kernel_boundary', 'test_memory_benchmark_plan', 'test_memory_benchmark_corpus')) {
        [IO.File]::WriteAllText((Join-Path $tools "$name.ps1"),
            "[IO.File]::AppendAllText(`$env:FLITZZIP_DRIVER_FIXTURE_LOG, '$name' + [Environment]::NewLine)")
    }
    [IO.File]::WriteAllText((Join-Path $tools 'cmake_toolchain.ps1'), @'
function Find-CMake { param([string]$RepoRoot); return (Join-Path $RepoRoot 'build/Release/cmake.exe') }
'@)
    [IO.File]::WriteAllText((Join-Path $tools 'version.ps1'), @'
function Read-FlitzZipCMakeCacheValue {
    param([string]$BuildRoot, [string]$Name)
    if ($Name -eq 'FLITZZIP_ENABLE_HIP') { return 'ON' }
    return '1.2.3'
}
function Assert-FlitzZipPackageVersionMatchesBuild { param([string]$BuildRoot, [string]$PackageVersion) }
function Resolve-FlitzZipPackageVersion { param([string]$RepoRoot, [string]$RequestedVersion); throw 'Invalid fixture version' }
'@)
    [IO.File]::WriteAllText((Join-Path $tools 'native_build_receipt.py'), @'
import os, pathlib, sys
with pathlib.Path(os.environ['FLITZZIP_DRIVER_FIXTURE_LOG']).open('a') as output:
    output.write('receipt\t' + '\t'.join(sys.argv[1:]) + '\n')
if os.environ.get('FLITZZIP_DRIVER_FIXTURE_FAILURE') == 'receipt':
    raise SystemExit(9)
print('{}')
'@)
}

function Assert-NativeDriverCase {
    param([string]$Root, [switch]$CorrectnessOnly, [string]$Failure = '')
    $log = Join-Path $Root 'commands.log'
    [IO.File]::WriteAllText($log, '')
    $oldLog = $env:FLITZZIP_DRIVER_FIXTURE_LOG
    $oldFailure = $env:FLITZZIP_DRIVER_FIXTURE_FAILURE
    try {
        $env:FLITZZIP_DRIVER_FIXTURE_LOG = $log
        $env:FLITZZIP_DRIVER_FIXTURE_FAILURE = $Failure
        $arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $Root 'tools/test.ps1') + '"'
        if ($CorrectnessOnly.IsPresent) { $arguments += ' -CorrectnessOnly' }
        $result = Invoke-NativeContractProcess -Runner (Get-Process -Id $PID).Path -Arguments $arguments
    } finally {
        $env:FLITZZIP_DRIVER_FIXTURE_LOG = $oldLog
        $env:FLITZZIP_DRIVER_FIXTURE_FAILURE = $oldFailure
    }
    $commands = @(Get-Content -LiteralPath $log)
    $ctest = @($commands | Where-Object { $_ -match '^ctest\t' -and $_ -notmatch '\t--show-only=json-v1' })
    $inventoryQueries = @($commands | Where-Object { $_ -match '^ctest\t.*\t--show-only=json-v1' })
    $runner = @($commands | Where-Object { $_ -ceq "flitzzip_tests`t--correctness-only" })
    $receipts = @($commands | Where-Object { $_ -match '^receipt\t' })
    $corpus = @($commands | Where-Object { $_ -ceq 'test_memory_benchmark_corpus' })
    if ($Failure) {
        if ($result.exit_code -eq 0 -or $corpus.Count -or $commands -contains 'test_memory_benchmark_plan' -or
            ($Failure -in @('receipt', 'gpu-info', 'ctest-inventory', 'inventory-role') -and $ctest.Count) -or
            ($Failure -in @('ctest', 'ctest-inventory', 'inventory-role') -and $runner.Count)) {
            throw "Native driver lost failure isolation: $Failure; $($result.stdout) $($result.stderr)"
        }
        return
    }
    $required = @('test_hip_architecture', 'test_rocm_toolchain', 'test_process_environment',
        'test_cmake_toolchain', 'test_hip_kernel_boundary', 'test_memory_benchmark_plan')
    if ($result.exit_code -ne 0 -or $ctest.Count -ne 1 -or
        @($required | Where-Object { $_ -notin $commands }).Count -or
        @($commands | Where-Object { $_ -ceq "flitzzip_cli`t--version" }).Count -ne 1 -or
        @($commands | Where-Object { $_ -ceq "flitzzip_cli`t--help" }).Count -ne 1) {
        throw "Native driver lost required consumers: $($result.stdout) $($result.stderr)"
    }
    if ($CorrectnessOnly.IsPresent) {
        if ($runner.Count -ne 1 -or $receipts.Count -ne 1 -or $receipts[0] -notmatch '\t--require-hip$' -or
            $ctest[0] -notmatch '\t-L\t\^correctness\$$' -or $inventoryQueries.Count -ne 1 -or $corpus.Count -or
            @($commands | Where-Object { $_ -ceq "flitzzip_cli`tgpu-info" }).Count -ne 2 -or
            $result.stdout -notmatch 'DEFERRED CTest measurement suite: measured' -or
            $result.stdout -notmatch 'DEFERRED: measured corpus/controller integration') {
            throw 'Correctness-only driver lost admission, exact CTest separation or explicit measurement deferral.'
        }
    } elseif ($runner.Count -or $receipts.Count -or $inventoryQueries.Count -or $ctest[0] -match '\t-L\t' -or $corpus.Count -ne 1) {
        throw 'Default native driver must retain full CTest and measured corpus/controller integration.'
    }
}

# Removing either separator must make the same recording oracle fail. Only the
# disposable copy is changed; no mutant can reach product executables or data.
function Assert-NativeDriverMutationRejected {
    param([string]$Root)
    $path = Join-Path $Root 'tools/test.ps1'
    $original = [IO.File]::ReadAllText($path)
    $corpusGuard = $original.LastIndexOf('if ($CorrectnessOnly.IsPresent) {', [StringComparison]::Ordinal)
    if ($corpusGuard -lt 0 -or -not $original.Contains(" -L '^correctness`$'")) {
        throw 'Driver mutation control no longer matches the reviewed separators.'
    }
    $mutants = @(
        $original.Remove($corpusGuard, 'if ($CorrectnessOnly.IsPresent) {'.Length).Insert($corpusGuard, 'if ($false) {'),
        $original.Replace(" -L '^correctness`$'", '')
    )
    try {
        foreach ($mutant in $mutants) {
            [IO.File]::WriteAllText($path, $mutant)
            $rejected = $false
            try { Assert-NativeDriverCase -Root $Root -CorrectnessOnly } catch {
                if ($_.Exception.Message -notlike 'Correctness-only driver lost admission*') { throw }
                $rejected = $true
            }
            if (-not $rejected) { throw 'Native driver oracle admitted an executable measurement-separation defect.' }
        }
    } finally { [IO.File]::WriteAllText($path, $original) }
    Assert-NativeDriverCase -Root $Root -CorrectnessOnly
}

try {
    $source = @'
#include "test_util.hpp"
#include <stdexcept>
struct Cases {
    Cases() {
        register_test("sample", [] {});
        register_test("sample_longer", [] {});
        register_test("failing", [] { throw std::runtime_error("fixture failure"); });
    }
} cases;
'@
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'cases.cpp'), $source)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'roles.cpp'), @'
#include "test_util.hpp"
TEST_CASE(correctness_control) {}
TEST_CASE(measurement_control, TestExecutionRole::Measurement) {
    throw std::runtime_error("measurement fixture executed");
}
'@)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'invalid.cpp'), @'
#include "test_util.hpp"
TEST_CASE(valid_before_invalid) {}
namespace {
struct InvalidRole {
    InvalidRole() {
        register_test("invalid_role", [] {}, static_cast<TestExecutionRole>(42));
    }
};
const InvalidRole invalid_role;
}
'@)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'null.cpp'), @'
#include "test_util.hpp"
namespace {
struct NullCallback {
    NullCallback() { register_test("null_callback", nullptr); }
};
const NullCallback null_callback;
}
'@)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'driver.cpp'), @'
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
    const char* path = std::getenv("FLITZZIP_DRIVER_FIXTURE_LOG");
    if (path == nullptr) { return 7; }
    const auto name = std::filesystem::path(argv[0]).stem().string();
    {
        std::ofstream output(path, std::ios::app);
        output << name;
        for (int i = 1; i < argc; ++i) { output << '\t' << argv[i]; }
        output << '\n';
        if (!output) { return 7; }
    }
    const char* failure = std::getenv("FLITZZIP_DRIVER_FIXTURE_FAILURE");
    if (failure != nullptr && name == failure && name != "ctest") { return 9; }
    if (name == "ctest") {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) != "--show-only=json-v1") { continue; }
            if (failure != nullptr && std::string_view(failure) == "ctest-inventory") { return 9; }
            const auto role = failure != nullptr && std::string_view(failure) == "inventory-role" ? "unknown" : "correctness";
            std::cout << R"({"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[)"
                << R"({"name":"flitzzip_tests","properties":[{"name":"LABELS","value":["mixed-native"]}]},)"
                << R"({"name":"fixture","properties":[{"name":"LABELS","value":[")" << role << R"("]}]},)"
                << R"({"name":"measured","properties":[{"name":"LABELS","value":["measurement"]}]}]})";
            return 0;
        }
        return failure != nullptr && std::string_view(failure) == "ctest" ? 9 : 0;
    }
    if (name == "flitzzip_tests") {
        if (argc == 2 && std::string_view(argv[1]) == "--correctness-only") { return 0; }
        std::cerr << "Invalid fixture selection\n";
        return 2;
    }
    if (name == "flitzzip_cli" && argc == 2) {
        const std::string_view command(argv[1]);
        if (command == "gpu-info") {
            if (failure != nullptr && std::string_view(failure) == "gpu-info") { return 9; }
            if (failure != nullptr && std::string_view(failure) == "gpu-after") {
                std::ifstream input(path);
                std::size_t probes = 0;
                for (std::string line; std::getline(input, line);) {
                    if (line == "flitzzip_cli\tgpu-info") { ++probes; }
                }
                if (probes > 1) { std::cout << "hip_compiled=true\navailable=false\n"; return 1; }
            }
            std::cout << "hip_compiled=true\navailable=true\n";
            return 0;
        }
        if (command == "--version") { std::cout << "FlitzZip 1.2.3\n"; return 0; }
        if (command == "--help") { std::cout << "flitzzip_cli --version\n"; return 0; }
    }
    return 7;
}
'@)
    $cmakeSource = @'
cmake_minimum_required(VERSION 3.20)
project(flitzzip_native_runner_contract LANGUAGES CXX)
if(NOT IS_ABSOLUTE "${FLITZZIP_TEST_SOURCE_DIR}" OR
   NOT EXISTS "${FLITZZIP_TEST_SOURCE_DIR}/test_main.cpp" OR
   NOT EXISTS "${FLITZZIP_TEST_SOURCE_DIR}/test_util.hpp")
    message(FATAL_ERROR "The production native test source directory is required")
endif()
# Compile original sources in place so quoted includes retain their real dependency layout.
add_library(runner_main OBJECT "${FLITZZIP_TEST_SOURCE_DIR}/test_main.cpp")
target_compile_features(runner_main PRIVATE cxx_std_20)
foreach(pair IN ITEMS "runner;cases.cpp" "runner_roles;roles.cpp" "runner_invalid_role;invalid.cpp" "runner_null;null.cpp")
    list(GET pair 0 target)
    list(GET pair 1 source)
    add_executable(${target} $<TARGET_OBJECTS:runner_main> "${source}")
    target_include_directories(${target} PRIVATE "${FLITZZIP_TEST_SOURCE_DIR}")
    target_compile_features(${target} PRIVATE cxx_std_20)
endforeach()
add_executable(driver_probe driver.cpp)
target_compile_features(driver_probe PRIVATE cxx_std_20)
'@
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), $cmakeSource)
    $build = Join-Path $fixtureRoot 'build'
    $cmake = Find-CMake -RepoRoot $root
    $generator = Find-CMakeGenerator -Requested $env:FLITZZIP_CMAKE_GENERATOR -BuildRoot $build
    $testSource = Join-Path $root 'tests/cpp'
    $configure = @(& $cmake -S $fixtureRoot -B $build -G $generator -A x64 "-DFLITZZIP_TEST_SOURCE_DIR=$testSource")
    if ($LASTEXITCODE -ne 0) { $configure; throw 'Native runner fixture configuration failed.' }
    $jobs = Resolve-BuildParallelism
    $compile = @(Invoke-FlitzZipParallelBuild -Jobs $jobs -Action {
        & $cmake --build $build --config Release --parallel $jobs
    })
    if ($LASTEXITCODE -ne 0) { $compile; throw 'Native runner fixture compilation failed.' }
    $runner = Join-Path $build 'Release/runner.exe'
    Assert-NativeRunnerCase -Runner $runner -Arguments '' -Exit 1 -Names @(
        'test_name_filter_modes_preserve_selection_boundaries',
        'test_name_filter_empty_anchored_requests_are_rejected', 'sample', 'sample_longer', 'failing')
    Assert-NativeRunnerCase -Runner $runner -Arguments 'sample' -Exit 0 -Names @('sample', 'sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=sample' -Exit 0 -Names @('sample')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=sample_longer' -Exit 0 -Names @('sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=failing' -Exit 1 -Names @('failing')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=missing' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments '^sample' -Exit 0 -Names @('sample', 'sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '^sample_' -Exit 0 -Names @('sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '^missing' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments '=' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments '^' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments 'sample second' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments '--correctness-only' -Exit 1 -Names @(
        'test_name_filter_modes_preserve_selection_boundaries',
        'test_name_filter_empty_anchored_requests_are_rejected', 'sample', 'sample_longer', 'failing')
    $rolesRunner = Join-Path $build 'Release/runner_roles.exe'
    Assert-NativeRunnerCase -Runner $rolesRunner -Arguments '' -Exit 1 -Names @(
        'test_name_filter_modes_preserve_selection_boundaries',
        'test_name_filter_empty_anchored_requests_are_rejected', 'correctness_control', 'measurement_control')
    Assert-NativeRunnerCase -Runner $rolesRunner -Arguments '--correctness-only' -Exit 0 -Names @(
        'test_name_filter_modes_preserve_selection_boundaries',
        'test_name_filter_empty_anchored_requests_are_rejected', 'correctness_control')
    Assert-NativeRunnerCase -Runner $rolesRunner -Arguments '=measurement_control' -Exit 1 -Names @('measurement_control')
    Assert-NativeRunnerCase -Runner $rolesRunner -Arguments '--correctness-only measurement_control' -Exit 2 -Names @()
    $invalidRunner = Join-Path $build 'Release/runner_invalid_role.exe'
    Assert-NativeRunnerCase -Runner $invalidRunner -Arguments '' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $invalidRunner -Arguments '--correctness-only' -Exit 2 -Names @()
    $nullRunner = Join-Path $build 'Release/runner_null.exe'
    Assert-NativeRunnerCase -Runner $nullRunner -Arguments '' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $nullRunner -Arguments '--correctness-only' -Exit 2 -Names @()
    $driverRoot = Join-Path $fixtureRoot 'driver'
    New-NativeDriverFixture -Root $driverRoot -Probe (Join-Path $build 'Release/driver_probe.exe') -Confirm:$false
    Assert-NativeDriverCase -Root $driverRoot
    Assert-NativeDriverCase -Root $driverRoot -CorrectnessOnly
    Assert-NativeDriverCase -Root $driverRoot -Failure 'ctest'
    foreach ($failure in @('receipt', 'gpu-info', 'ctest-inventory', 'inventory-role', 'ctest', 'flitzzip_tests', 'gpu-after')) {
        Assert-NativeDriverCase -Root $driverRoot -CorrectnessOnly -Failure $failure
    }
    Assert-NativeDriverMutationRejected -Root $driverRoot
    Write-Output 'Production native runner passed selection, role retention, measurement deferral and fail-closed registration controls.'
} finally {
    $prefix = $ownedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixtureRoot).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified native runner fixture.'
    }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
