# Purpose: Qualify standard-only private PCH ownership, mixed-language compilation and disabled-PCH parity.
# Inputs: Canonical CMake module and bounded native fixtures; actual installed MSVC and pinned CMake.
# Outputs: Builds and runs both strategies without touching the product build; fails on any contract difference.
param()
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
. (Join-Path $PSScriptRoot 'build_parallelism.ps1')
$taskCmake = Find-CMake -RepoRoot $taskRepo
$taskValidationRoot = Join-Path $taskRepo 'out/precompiled-header-contracts'
$taskValidation = Join-Path $taskValidationRoot ([guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $taskValidation) { throw 'PCH validation requires a fresh owned directory.' }
foreach ($taskPath in @((Join-Path $taskRepo 'out'), (Split-Path -Parent $taskValidation), $taskValidation)) {
    if ((Test-Path -LiteralPath $taskPath) -and
        ((Get-Item -LiteralPath $taskPath).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing a linked PCH validation directory.'
    }
}
$taskGenerator = Find-CMakeGenerator -Requested '' -BuildRoot $taskValidation
$taskJobs = Resolve-BuildParallelism
Invoke-FlitzZipBackgroundWork {
    foreach ($taskDisabled in @('FALSE', 'TRUE')) {
        $taskBuild = Join-Path $taskValidation $taskDisabled
        if ((Test-Path -LiteralPath $taskBuild) -and
            ((Get-Item -LiteralPath $taskBuild).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing a linked PCH contract build directory.'
        }
        & $taskCmake -S (Join-Path $taskRepo 'tests/cmake/precompiled_headers') -B $taskBuild `
            -G $taskGenerator -A x64 "-DREPO_ROOT=$taskRepo" "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=$taskDisabled"
        if ($LASTEXITCODE -ne 0) { throw 'Precompiled-header contract configure failed.' }
        Invoke-FlitzZipParallelBuild -Jobs $taskJobs -Action {
            & $taskCmake --build $taskBuild --config Release --parallel $taskJobs
            if ($LASTEXITCODE -ne 0) { throw 'Precompiled-header contract build failed.' }
        }
        foreach ($taskTarget in @('flitzzip_core', 'flitzzip_test_objects', 'FlitzZip', 'decoy')) {
            & (Join-Path $taskBuild "Release/$taskTarget.exe")
            if ($LASTEXITCODE -ne 0) { throw 'Precompiled-header runtime or target-local policy changed.' }
        }
        $taskPchCount = @(Get-ChildItem -LiteralPath $taskBuild -Recurse -File -Filter '*.pch').Count
        if (($taskDisabled -eq 'TRUE' -and $taskPchCount -ne 0) -or
            ($taskDisabled -eq 'FALSE' -and $taskPchCount -ne 3)) {
            throw 'Unexpected native PCH artifact ownership or disabled-build behavior.'
        }
    }
}
Write-Output 'Private C++ PCH and disabled-PCH contracts passed for all independent targets.'
$taskResolved = [IO.Path]::GetFullPath($taskValidation)
$taskAllowed = [IO.Path]::GetFullPath($taskValidationRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $taskResolved.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'PCH cleanup target escapes its owned validation root.'
}
Remove-Item -LiteralPath $taskResolved -Recurse -Force
