param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$PackageVersion = "",
    [switch]$CreateMsi
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
$stage = Join-Path $repo "out\install-$Configuration"

# Purpose: Resolve the package display version from an explicit argument or CMake project metadata.
# Inputs: RequestedVersion is an optional SemVer string supplied by the caller.
# Outputs: Returns the version string used in portable ZIP and MSI filenames.
function Resolve-PackageVersion {
    param([string]$RequestedVersion)
    if ($RequestedVersion) {
        return $RequestedVersion
    }
    $cmakeLists = Join-Path $repo "CMakeLists.txt"
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $match = [regex]::Match($text, "project\s*\(\s*SuperZip\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", "IgnoreCase")
    if (-not $match.Success) {
        throw "Unable to resolve package version from CMakeLists.txt."
    }
    return $match.Groups[1].Value
}

$PackageVersion = Resolve-PackageVersion -RequestedVersion $PackageVersion
$packageBase = "SuperZip-$PackageVersion-win64"
$package = Join-Path $repo "out\$packageBase-portable.zip"

# Purpose: Find a usable CMake executable on a Windows development or CI host.
# Inputs: None; probes known install paths and PATH.
# Outputs: Returns the CMake executable path or throws when CMake is unavailable.
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

$zipHash = Get-FileHash -Algorithm SHA256 -LiteralPath $package
Set-Content -LiteralPath "$package.sha256" -Value "$($zipHash.Hash.ToLowerInvariant())  $(Split-Path -Leaf $package)"
Write-Host "Created $package.sha256"

if ($CreateMsi) {
    $cpack = Join-Path (Split-Path -Parent $cmake) "cpack.exe"
    if (-not (Test-Path $cpack)) {
        throw "CPack was not found next to CMake at $cmake."
    }
    $wix = Get-Command wix -ErrorAction SilentlyContinue
    if (-not $wix) {
        throw "WiX was not found. Install the WiX .NET tool before requesting MSI packaging."
    }
    & $cpack -G WIX -C $Configuration --config (Join-Path $build "CPackConfig.cmake") -B (Join-Path $repo "out")
    $msi = Join-Path $repo "out\$packageBase.msi"
    if (-not (Test-Path $msi)) {
        throw "Expected MSI was not produced: $msi"
    }
    $msiHash = Get-FileHash -Algorithm SHA256 -LiteralPath $msi
    Set-Content -LiteralPath "$msi.sha256" -Value "$($msiHash.Hash.ToLowerInvariant())  $(Split-Path -Leaf $msi)"
    Write-Host "Created $msi"
    Write-Host "Created $msi.sha256"
}
