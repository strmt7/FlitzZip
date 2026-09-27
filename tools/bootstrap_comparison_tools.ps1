param(
    [string]$ToolRoot = '',
    [switch]$Offline
)

# Purpose: Provision checksum-pinned independent archive readers for the size comparison harness.
# Inputs: An optional repository-owned output root and an offline cache-only mode.
# Outputs: Verified 7-Zip and Zstandard executables under the ignored output tree.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$outRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out'))
if (-not $ToolRoot) { $ToolRoot = Join-Path $outRoot 'tools' }
$toolRoot = [IO.Path]::GetFullPath($ToolRoot)
if (-not $toolRoot.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar,
                              [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Comparison tools must remain inside the repository ignored out directory.'
}
New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
if ((Get-Item -LiteralPath $toolRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Refusing a linked comparison tool root.'
}

# Purpose: Require exact bytes for a pinned distribution or executable.
# Inputs: An existing file path, its expected SHA-256, and a diagnostic label.
# Outputs: Returns on a match; throws on missing or modified content.
function Assert-ComparisonHash {
    param([string]$Path, [string]$Expected, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label is missing: $Path" }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) { throw "$Label SHA-256 mismatch: $Path" }
}

# Purpose: Obtain one immutable release asset without accepting a stale or modified cache file.
# Inputs: Official asset URL, expected SHA-256, cache path, and offline policy.
# Outputs: Returns the verified cache path or throws before extraction.
function Get-ComparisonAsset {
    param([string]$Uri, [string]$Expected, [string]$Path, [bool]$CacheOnly)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($CacheOnly) { throw "Offline comparison asset is missing: $Path" }
        $partial = "$Path.partial"
        if (Test-Path -LiteralPath $partial) { throw "Incomplete comparison asset already exists: $partial" }
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $partial
            Assert-ComparisonHash -Path $partial -Expected $Expected -Label 'Downloaded comparison asset'
            Move-Item -LiteralPath $partial -Destination $Path
        } catch {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
            throw
        }
    }
    Assert-ComparisonHash -Path $Path -Expected $Expected -Label 'Cached comparison asset'
    return $Path
}

$sevenDir = Join-Path $toolRoot '7zip-26.03\x64'
$sevenFiles = @(
    @('7za.exe', 'EDBEE35370E14030E4C785CF88200F42DC651C1EB4217C1E3963C38A12F099B0'),
    @('7za.dll', '876077825E49F5A39FB472532080B91A41FEC508C714EAB8A51AF5A51183EBA0'),
    @('7zxa.dll', '6509B5D4895103250405AB516421B4068E78B67CABC0C6EEF774DF31572D9FAD')
)
$zstdDir = Join-Path $toolRoot 'zstd-v1.5.7-win64'
$zstdExe = Join-Path $zstdDir 'zstd.exe'
$zstdExeHash = '8076AAE03FEAC7C66B319579E82172EED168DEED2A3F25E5E2D3C60F55E84111'
$sevenReady = Test-Path -LiteralPath $sevenDir -PathType Container
if ($sevenReady) {
    foreach ($entry in $sevenFiles) {
        Assert-ComparisonHash -Path (Join-Path $sevenDir $entry[0]) -Expected $entry[1] -Label "7-Zip $($entry[0])"
    }
}
$zstdReady = Test-Path -LiteralPath $zstdDir -PathType Container
if ($zstdReady) {
    Assert-ComparisonHash -Path $zstdExe -Expected $zstdExeHash -Label 'Zstandard executable'
}
if ($sevenReady -and $zstdReady) {
    Write-Output "comparison_tools status=verified root=$toolRoot"
    return
}

$cache = Join-Path $toolRoot 'downloads'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$stageRoot = Join-Path $toolRoot 'bootstrap-stage'
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
foreach ($ownedDirectory in @($cache, $stageRoot)) {
    if ((Get-Item -LiteralPath $ownedDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing a linked comparison tool directory: $ownedDirectory"
    }
}
$stage = Join-Path $stageRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    if (-not $sevenReady) {
        $release = 'https://github.com/ip7z/7zip/releases/download/26.03/'
        $extractor = Get-ComparisonAsset -Uri ($release + '7zr.exe') `
            -Expected 'AD4C82FADCBDF93C03B4FC440F300509C7D60C5C2F4D183E35D9D70D6957037D' `
            -Path (Join-Path $cache '7zr-26.03.exe') -CacheOnly $Offline.IsPresent
        $archive = Get-ComparisonAsset -Uri ($release + '7z2603-extra.7z') `
            -Expected '191894E6ACB3647FFB69CE630479FF318523B2E2B9890AA7F05C1127C2E59B8F' `
            -Path (Join-Path $cache '7z2603-extra.7z') -CacheOnly $Offline.IsPresent
        & $extractor x -y "-o$stage" $archive 'x64/7za.exe' 'x64/7za.dll' 'x64/7zxa.dll' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip asset extraction failed: $LASTEXITCODE" }
        foreach ($entry in $sevenFiles) {
            Assert-ComparisonHash -Path (Join-Path $stage "x64\$($entry[0])") -Expected $entry[1] -Label "Staged 7-Zip $($entry[0])"
        }
        $sevenParent = Split-Path -Parent $sevenDir
        if (Test-Path -LiteralPath $sevenParent) { throw "Incomplete 7-Zip installation exists: $sevenParent" }
        New-Item -ItemType Directory -Path $sevenParent | Out-Null
        Move-Item -LiteralPath (Join-Path $stage 'x64') -Destination $sevenDir
    }
    if (-not $zstdReady) {
        $zip = Join-Path $repo 'third_party\upstream\zstd\v1.5.7\zstd-v1.5.7-win64.zip'
        Assert-ComparisonHash -Path $zip `
            -Expected 'ACB4E8111511749DC7A3EBEDCA9B04190E37A17AFEB73F55D4425DBF0B90FAD9' `
            -Label 'Pinned Zstandard distribution'
        & tar.exe -xf $zip -C $stage 'zstd-v1.5.7-win64/zstd.exe'
        if ($LASTEXITCODE -ne 0) { throw "Zstandard asset extraction failed: $LASTEXITCODE" }
        $stagedZstd = Join-Path $stage 'zstd-v1.5.7-win64\zstd.exe'
        Assert-ComparisonHash -Path $stagedZstd -Expected $zstdExeHash -Label 'Staged Zstandard executable'
        if (Test-Path -LiteralPath $zstdDir) { throw "Incomplete Zstandard installation exists: $zstdDir" }
        Move-Item -LiteralPath (Join-Path $stage 'zstd-v1.5.7-win64') -Destination $zstdDir
    }
} finally {
    $verifiedStage = [IO.Path]::GetFullPath($stage)
    $verifiedRoot = [IO.Path]::GetFullPath($stageRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $verifiedStage.StartsWith($verifiedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to clean an unverified comparison staging path.'
    }
    Remove-Item -LiteralPath $verifiedStage -Recurse -Force
}
Write-Output "comparison_tools status=provisioned root=$toolRoot"
