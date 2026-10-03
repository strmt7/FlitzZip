$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
$root = Split-Path -Parent $PSScriptRoot
$ownedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/native-runner-contracts'))
$fixtureRoot = Join-Path $ownedRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

# Purpose: Exercise the production native runner against a controlled three-case registry.
# Inputs: Runner is the fixture executable; Arguments are literal filters; Exit/Names are expected results.
# Outputs: Rejects wrong selection, lost failures, unexpected execution or an owned-process timeout.
function Assert-NativeRunnerCase {
    param([string]$Runner, [string]$Arguments, [int]$Exit, [string[]]$Names)
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
        $actual = @([regex]::Matches($text, '(?m)^\[RUN \] (?<name>\w+)\r?$') |
            ForEach-Object { $_.Groups['name'].Value })
        if ($process.ExitCode -ne $Exit -or ($actual -join '|') -ne ($Names -join '|') -or
            ($Exit -eq 2 -and [string]::IsNullOrWhiteSpace($errorText))) {
            throw "Native runner contract failed for '$Arguments': exit=$($process.ExitCode), cases=$($actual -join ','); $text $errorText"
        }
    } finally { $process.Dispose() }
}

try {
    Copy-Item -LiteralPath (Join-Path $root 'tests/cpp/test_main.cpp') -Destination (Join-Path $fixtureRoot 'test_main.cpp')
    $source = @'
#include <functional>
#include <stdexcept>
#include <string>
void register_test(std::string, std::function<void()>);
struct Cases {
    Cases() {
        register_test("sample", [] {});
        register_test("sample_longer", [] {});
        register_test("failing", [] { throw std::runtime_error("fixture failure"); });
    }
} cases;
'@
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'cases.cpp'), $source)
    $cmakeSource = @'
cmake_minimum_required(VERSION 3.20)
project(superzip_native_runner_contract LANGUAGES CXX)
add_executable(runner test_main.cpp cases.cpp)
target_compile_features(runner PRIVATE cxx_std_20)
'@
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), $cmakeSource)
    $build = Join-Path $fixtureRoot 'build'
    $cmake = Find-CMake -RepoRoot $root
    $generator = Find-CMakeGenerator -Requested $env:SUPERZIP_CMAKE_GENERATOR -BuildRoot $build
    $configure = @(& $cmake -S $fixtureRoot -B $build -G $generator -A x64)
    if ($LASTEXITCODE -ne 0) { $configure; throw 'Native runner fixture configuration failed.' }
    $compile = @(& $cmake --build $build --config Release --parallel)
    if ($LASTEXITCODE -ne 0) { $compile; throw 'Native runner fixture compilation failed.' }
    $runner = Join-Path $build 'Release/runner.exe'
    Assert-NativeRunnerCase -Runner $runner -Arguments '' -Exit 1 -Names @('sample', 'sample_longer', 'failing')
    Assert-NativeRunnerCase -Runner $runner -Arguments 'sample' -Exit 0 -Names @('sample', 'sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=sample' -Exit 0 -Names @('sample')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=sample_longer' -Exit 0 -Names @('sample_longer')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=failing' -Exit 1 -Names @('failing')
    Assert-NativeRunnerCase -Runner $runner -Arguments '=missing' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments '=' -Exit 2 -Names @()
    Assert-NativeRunnerCase -Runner $runner -Arguments 'sample second' -Exit 2 -Names @()
    Write-Output 'Production native runner passed eight default, substring, exact and rejection contracts.'
} finally {
    $prefix = $ownedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixtureRoot).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified native runner fixture.'
    }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
