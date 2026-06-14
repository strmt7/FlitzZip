param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
$stage = Join-Path $repo "out\install-$Configuration"
$package = Join-Path $repo "out\SuperZip-$Configuration.zip"

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

if (-not (Test-Path $build)) {
    throw "Build directory not found. Run tools/build.ps1 first."
}

if (Test-Path $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$cmake = Find-CMake
& $cmake --install $build --config $Configuration --prefix $stage

if (Test-Path $package) {
    Remove-Item -LiteralPath $package -Force
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $package) | Out-Null
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $package -CompressionLevel Optimal
Write-Host "Created $package"
