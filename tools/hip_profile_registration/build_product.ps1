param(
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$Python
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools/cmake_toolchain.ps1')
. (Join-Path $repo 'tools/build_parallelism.ps1')
$previous = Get-Location
try {
    Set-Location -LiteralPath $repo
    & $Python -B -m tools.hip_coverage_measure admit --manifest $Manifest --build-root $BuildRoot
    if ($LASTEXITCODE -ne 0) { throw 'Development device coverage build admission failed.' }
    $gui = Join-Path $BuildRoot 'Debug/FlitzZip.exe'
    foreach ($process in @(Get-Process -Name FlitzZip -ErrorAction SilentlyContinue)) {
        if ($process.Path -eq $gui) { throw 'Close this development build GUI before building it.' }
    }
    $cmake = Find-CMake -RepoRoot $repo
    Invoke-FlitzZipBackgroundWork {
        $jobs = Resolve-BuildParallelism
        Invoke-FlitzZipParallelBuild -Jobs $jobs -Action {
            & $cmake --build $BuildRoot --config Debug --target flitzzip_tests --parallel $jobs
            if ($LASTEXITCODE -ne 0) { throw 'Development device coverage build failed; retain diagnostics.' }
        }
    }
} finally {
    Set-Location -LiteralPath $previous.Path
}
