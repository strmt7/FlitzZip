$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')

# Purpose: Require a tool-integrity failure without allowing an expected exception to pass silently.
# Inputs: Action is an isolated hash/cache admission check expected to throw.
# Outputs: Returns only when the action rejects its input; throws for unexpected acceptance.
function Assert-CMakeRejection {
    param([scriptblock]$Action)
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid CMake tool content was accepted.' }
}

$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo ('out/cmake-toolchain-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$fixture = Join-Path $root 'tool.bin'
$expected = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
try {
    Assert-CMakeRejection { Assert-CMakeToolHash -Path $fixture -Expected $expected }
    [IO.File]::WriteAllText($fixture, 'abc', [Text.UTF8Encoding]::new($false))
    Assert-CMakeToolHash -Path $fixture -Expected $expected
    [IO.File]::WriteAllText($fixture, 'abd', [Text.UTF8Encoding]::new($false))
    Assert-CMakeRejection { Assert-CMakeToolHash -Path $fixture -Expected $expected }
    $bin = Join-Path $root 'out/tools/cmake-4.4.3/cmake-4.4.3-windows-x86_64/bin'
    New-Item -ItemType Directory -Path $bin | Out-Null
    $fake = Join-Path $bin 'cmake.exe'
    [IO.File]::WriteAllText($fake, 'not an executable', [Text.UTF8Encoding]::new($false))
    Assert-CMakeRejection { Find-CMake -RepoRoot $root }
    Remove-Item -LiteralPath $fake
    foreach ($directory in @($bin, (Split-Path $bin), (Split-Path (Split-Path $bin)),
                             (Join-Path $root 'out/tools'), (Join-Path $root 'out'))) {
        Remove-Item -LiteralPath $directory
    }
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture }
    if (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 0) { Remove-Item -LiteralPath $root }
}
$cmake = Find-CMake -RepoRoot $repo
$version = & $cmake --version
if ($LASTEXITCODE -ne 0 -or $version[0] -ne 'cmake version 4.4.3') {
    throw 'The repository must use the verified CMake 4.4.3 toolchain.'
}
Write-Output 'CMake toolchain integrity tests passed.'

foreach ($case in @(
    @('', '', @(17), 'Visual Studio 17 2022'),
    @('', '', @(17, 18), 'Visual Studio 18 2026'),
    @('', 'Visual Studio 17 2022', @(17, 18), 'Visual Studio 17 2022'),
    @('Visual Studio 17 2022', '', @(17, 18), 'Visual Studio 17 2022'),
    @('Visual Studio 18 2026', 'Visual Studio 18 2026', @(18), 'Visual Studio 18 2026')
)) {
    if ((Select-CMakeGenerator -Requested $case[0] -Cached $case[1] -InstalledMajors $case[2]) -ne $case[3]) {
        throw 'CMake generator selection failed.'
    }
}
Assert-CMakeRejection { Select-CMakeGenerator -Requested '' -Cached '' -InstalledMajors @() }
Assert-CMakeRejection { Select-CMakeGenerator -Requested '' -Cached 'Visual Studio 17 2022' -InstalledMajors @(18) }
Assert-CMakeRejection {
    Select-CMakeGenerator -Requested 'Visual Studio 18 2026' -Cached 'Visual Studio 17 2022' -InstalledMajors @(17, 18)
}
Write-Output 'CMake generator selection tests passed.'

foreach ($entryPoint in @('build.ps1', 'test.ps1', 'package.ps1', 'test_msi_identity.ps1')) {
    $content = Get-Content -LiteralPath (Join-Path $PSScriptRoot $entryPoint) -Raw
    if (-not $content.Contains('"cmake_toolchain.ps1"') -or
        $content -match '(?m)^function Find-CMake\b|CommonExtensions\\Microsoft\\CMake') {
        throw "CMake entry point $entryPoint bypasses the shared pinned toolchain."
    }
}
Write-Output 'CMake production/test toolchain parity passed.'
