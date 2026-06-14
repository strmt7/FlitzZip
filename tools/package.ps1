param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$PackageVersion = "",
    [switch]$AllowCpuValidationPackage,
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

# Purpose: Find the WiX CLI from the repository-local tool cache or PATH.
# Inputs: None; probes the ignored local tools folder first, then PATH.
# Outputs: Returns wix.exe path or throws when WiX is unavailable.
function Find-Wix {
    $candidates = @(
        (Join-Path $repo "out\tools\wix\wix.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $cmd = Get-Command wix -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "WiX was not found. Install the pinned WiX .NET tool before requesting MSI packaging."
}

# Purpose: Ensure WiX v7 has an explicit maintainer EULA acceptance before MSI creation.
# Inputs: WixPath is the resolved wix.exe path.
# Outputs: Accepts the configured EULA for the current user profile or throws when the repository variable is absent.
function Confirm-WixEula {
    param([string]$WixPath)
    if ($env:SUPERZIP_ACCEPT_WIX_OSMF_EULA -ne "wix7") {
        throw "MSI packaging with WiX v7 requires SUPERZIP_ACCEPT_WIX_OSMF_EULA=wix7 after accepting the WiX OSMF EULA."
    }
    & $WixPath eula accept $env:SUPERZIP_ACCEPT_WIX_OSMF_EULA
    if ($LASTEXITCODE -ne 0) {
        throw "WiX EULA acceptance command failed."
    }
}

if (-not (Test-Path $build)) {
    throw "Build directory not found. Run tools/build.ps1 first."
}

$cachePath = Join-Path $build "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "CMake cache not found. Run tools/build.ps1 first."
}
$cacheLines = Get-Content -LiteralPath $cachePath
$hipEntry = $cacheLines | Where-Object { $_ -match '^SUPERZIP_ENABLE_HIP:[^=]+=.+$' } | Select-Object -First 1
$hipEnabled = $hipEntry -match '=ON$'
if (-not $hipEnabled -and -not $AllowCpuValidationPackage) {
    throw "Refusing to package a CPU-only SuperZip build. Rebuild with AMD HIP enabled, or pass -AllowCpuValidationPackage only for internal CI validation artifacts that will not be released."
}

if (Test-Path $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$cmake = Find-CMake
& $cmake --install $build --config $Configuration --prefix $stage

$cli = Join-Path $stage "bin\superzip_cli.exe"
if (-not (Test-Path -LiteralPath $cli)) {
    throw "Staged CLI was not produced: $cli"
}
if ($hipEnabled) {
    $manifest = Join-Path $stage "superzip-runtime-dependencies.json"
    if (-not (Test-Path -LiteralPath $manifest)) {
        throw "HIP runtime dependency manifest was not staged: $manifest"
    }
    if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $PSNativeCommandUseErrorActionPreference = $false
    }
    $dependencyOutput = & $cli dependency-check 2>&1
    $dependencyExit = $LASTEXITCODE
    $dependencyOutput | Set-Content -LiteralPath (Join-Path $stage "superzip-dependency-check.txt")
    if ($dependencyOutput -notcontains "hip_compiled=true") {
        throw "Staged CLI is not reporting a HIP-enabled build."
    }
    if ($dependencyOutput -notcontains "hip_runtime_loadable=true") {
        throw "Staged CLI cannot load the AMD HIP runtime on this build host. Install/update the AMD GPU driver or ensure the HIP SDK bin directory is discoverable before packaging."
    }
    if ($dependencyExit -notin @(0, 12)) {
        throw "Staged CLI dependency check failed with exit code $dependencyExit."
    }
}

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
    $wix = Find-Wix
    Confirm-WixEula -WixPath $wix
    $env:PATH = "$(Split-Path -Parent $wix);$env:PATH"
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
