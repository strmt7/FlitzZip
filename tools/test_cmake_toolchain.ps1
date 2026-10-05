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
$fixtureBytes = [Text.Encoding]::ASCII.GetBytes('abc')
$fixtureHasher = [Security.Cryptography.SHA256]::Create()
try { $expected = ([BitConverter]::ToString($fixtureHasher.ComputeHash($fixtureBytes))).Replace('-', '') }
finally { $fixtureHasher.Dispose() }
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

# Purpose: Verify source-bound ABI header repair and compiled C/C++ payloads.
# Inputs: The verified toolchain and current checkout; fixtures are private.
# Outputs: Rejects unknown bytes and interrupted writes, preserves idempotence,
# and validates original/repaired pointer-size and byte-order probes.
function Test-CMakeCompilerAbiRepair {
    param([string]$RepoRoot, [string]$CMake)
    $fixtureRoot = Join-Path $RepoRoot ('out/cmake-abi-contract-' + [guid]::NewGuid().ToString('N'))
    $fixtureInstall = Join-Path $fixtureRoot 'installation'
    $modules = Join-Path $fixtureInstall 'share/cmake-4.4/Modules'
    New-Item -ItemType Directory -Path $modules | Out-Null
    $header = Join-Path $modules 'CMakeCompilerABI.h'
    $install = Split-Path (Split-Path $CMake)
    $production = Join-Path $install 'share/cmake-4.4/Modules/CMakeCompilerABI.h'
    $patched = [IO.File]::ReadAllText($production)
    $prefix = "#ifndef SUPERZIP_CMAKE_COMPILER_ABI_H`n#define SUPERZIP_CMAKE_COMPILER_ABI_H`n"
    $suffix = "`n#endif /* SUPERZIP_CMAKE_COMPILER_ABI_H */`n"
    if (-not $patched.StartsWith($prefix) -or -not $patched.EndsWith($suffix)) {
        throw 'Production compiler ABI header is not completely guarded.'
    }
    $body = $patched.Substring($prefix.Length, $patched.Length - $prefix.Length - $suffix.Length)
    $original = Join-Path $fixtureRoot 'original.h'
    [IO.File]::WriteAllText($original, $body, [Text.UTF8Encoding]::new($false))
    Assert-CMakeRejection { Repair-CMakeCompilerAbiHeader -Install $fixtureInstall }
    [IO.File]::WriteAllText($header, 'unknown ABI probe', [Text.UTF8Encoding]::new($false))
    Assert-CMakeRejection { Repair-CMakeCompilerAbiHeader -Install $fixtureInstall }
    Copy-Item -LiteralPath $original -Destination $header
    $partial = "$header.superzip-abi"
    [IO.File]::WriteAllText($partial, 'interrupted publication', [Text.UTF8Encoding]::new($false))
    Assert-CMakeRejection { Repair-CMakeCompilerAbiHeader -Install $fixtureInstall }
    Remove-Item -LiteralPath $partial
    Repair-CMakeCompilerAbiHeader -Install $fixtureInstall
    if ([IO.File]::ReadAllText($header) -cne $patched) { throw 'Repaired ABI payload differs from production.' }
    $before = (Get-Item -LiteralPath $header).LastWriteTimeUtc
    Repair-CMakeCompilerAbiHeader -Install $fixtureInstall
    if ((Get-Item -LiteralPath $header).LastWriteTimeUtc -ne $before) { throw 'ABI repair is not idempotent.' }
    [IO.File]::AppendAllText($header, "`n/* repaired source drift */`n")
    Assert-CMakeRejection { Repair-CMakeCompilerAbiHeader -Install $fixtureInstall }
    [IO.File]::WriteAllText($header, $patched, [Text.UTF8Encoding]::new($false))
    $generator = Find-CMakeGenerator -BuildRoot (Join-Path $RepoRoot 'build')
    $arguments = @('-S', (Join-Path $RepoRoot 'tests/cmake/compiler_abi'), '-B',
        (Join-Path $fixtureRoot 'build'), '-G', $generator, '-A', 'x64',
        ('-DORIGINAL_HEADER=' + ($original -replace '\\', '/')),
        ('-DREPAIRED_HEADER=' + ($header -replace '\\', '/')))
    & $CMake @arguments
    if ($LASTEXITCODE -ne 0) { throw 'C/C++ compiler ABI probe contracts failed.' }
}

Test-CMakeCompilerAbiRepair -RepoRoot $repo -CMake $cmake
Write-Output 'CMake guarded ABI source and C/C++ payload contracts passed.'

# Purpose: Require the public checksum manifest's exact inventory and canonical records.
# Inputs: Private copies of the checked-in standard checksum data.
# Outputs: Rejects duplicates, unknown/missing records, malformed hashes and oversized content.
function Test-CMakeToolchainManifest {
    param([string]$RepoRoot)
    $fixture = Join-Path $RepoRoot ('out/cmake-manifest-' + [guid]::NewGuid().ToString('N') + '.sha256')
    $original = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'cmake-toolchain.sha256'))
    $lines = $original.TrimEnd().Split("`n")
    try {
        [IO.File]::WriteAllText($fixture, $original)
        Get-CMakeToolchainLock -Path $fixture | Out-Null
        foreach ($mutation in @(
            ($original + $lines[0] + "`n"),
            (($lines | Select-Object -Skip 1) -join "`n"),
            ('x' + $original.Substring(1)),
            ($original + $lines[0].Replace('cmake-4.4.3-windows-x86_64.zip', 'unknown.exe') + "`n"),
            ('x' * 16385))) {
            [IO.File]::WriteAllText($fixture, $mutation)
            Assert-CMakeRejection { Get-CMakeToolchainLock -Path $fixture }
        }
    } finally {
        if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture }
    }
}

Test-CMakeToolchainManifest -RepoRoot $repo
Write-Output 'CMake public checksum manifest contracts passed.'
