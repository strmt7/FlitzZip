param(
    [string]$ToolRoot = '',
    [switch]$Offline,
    [ValidateSet('ArchiveReaders', 'Hyperfine')][string]$Tool = 'ArchiveReaders'
)

# Purpose: Provision checksum-pinned independent archive readers or the benchmark timing driver.
# Inputs: An optional repository-owned output root, tool selection and offline cache-only mode.
# Outputs: Verified 7-Zip/Zstandard or Hyperfine executables under the ignored output tree.
$ErrorActionPreference = 'Stop'
# Resolve inbox modules from this interpreter, independent of inherited PSModulePath entries.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Management/Microsoft.PowerShell.Management.psd1') -ErrorAction Stop
$repo = Split-Path -Parent $PSScriptRoot
$outRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out'))

# Purpose: Reject linked ancestors and escapes before creating, reading or publishing owned tool paths.
# Inputs: Candidate path and the repository-owned output boundary.
# Outputs: Returns for a canonical contained path; throws before following any existing reparse point.
function Assert-ComparisonOwnedPath {
    param([string]$Path, [string]$Boundary)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullBoundary = [IO.Path]::GetFullPath($Boundary).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not $fullPath.StartsWith($fullBoundary + [IO.Path]::DirectorySeparatorChar,
                                 [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Comparison tools must remain inside the repository ignored out directory.'
    }
    $ancestor = $fullPath
    while ($ancestor.Length -ge $fullBoundary.Length) {
        if ((Test-Path -LiteralPath $ancestor) -and
            ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing a linked comparison tool ancestor.'
        }
        $ancestor = Split-Path -Parent $ancestor
    }
}

if (-not $ToolRoot) { $ToolRoot = Join-Path $outRoot 'tools' }
$toolRoot = [IO.Path]::GetFullPath($ToolRoot)
Assert-ComparisonOwnedPath -Path $toolRoot -Boundary $outRoot
New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null

# Purpose: Require exact bytes for a pinned distribution or executable.
# Inputs: An existing file path, its expected SHA-256, and a diagnostic label.
# Outputs: Returns on a match; throws on missing or modified content.
function Assert-ComparisonHash {
    param([string]$Path, [string]$Expected, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label is missing: $Path" }
    if ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "$Label is linked: $Path"
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) { throw "$Label SHA-256 mismatch: $Path" }
}

# An absent installation can be provisioned. An existing installation must have
# every pinned executable and notice; corruption is never treated as a cache miss.
function Test-ComparisonInstallation {
    param([string]$Root, [object[]]$Files, [string]$Label)
    Assert-ComparisonOwnedPath -Path $Root -Boundary $outRoot
    if (-not (Test-Path -LiteralPath $Root)) { return $false }
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "$Label installation is not a directory: $Root"
    }
    foreach ($entry in $Files) {
        $path = Join-Path $Root $entry[0]
        Assert-ComparisonOwnedPath -Path $path -Boundary $Root
        Assert-ComparisonHash -Path $path -Expected $entry[1] -Label "$Label $($entry[0])"
    }
    return $true
}

# Supply a missing notice only for the previous exact single-binary layout.
# Existing notices must match their pin; no file is overwritten or removed.
function Restore-ComparisonNotice {
    param([string]$Root, [string]$BinaryName, [string]$BinaryHash,
          [string]$Source, [string]$NoticeName, [string]$NoticeHash)
    Assert-ComparisonOwnedPath -Path $Root -Boundary $outRoot
    if (-not (Test-Path -LiteralPath $Root)) { return }
    $notice = Join-Path $Root $NoticeName
    Assert-ComparisonOwnedPath -Path $notice -Boundary $Root
    if (Test-Path -LiteralPath $notice) {
        Assert-ComparisonHash -Path $notice -Expected $NoticeHash -Label 'Installed comparison notice'
        return
    }
    if (-not (Test-ComparisonInstallation -Root $Root -Files (,@($BinaryName, $BinaryHash)) -Label 'Legacy comparator')) {
        throw 'Legacy comparator installation is missing.'
    }
    $entries = @(Get-ChildItem -LiteralPath $Root -Force)
    if ($entries.Count -ne 1 -or $entries[0].Name -cne $BinaryName) {
        throw 'Unrecognized legacy comparator installation layout.'
    }
    Assert-ComparisonHash -Path $Source -Expected $NoticeHash -Label 'Comparison notice source'
    [IO.File]::Copy($Source, $notice)
    Assert-ComparisonHash -Path $notice -Expected $NoticeHash -Label 'Installed comparison notice'
}

# Purpose: Obtain one immutable release asset without accepting a stale or modified cache file.
# Inputs: Official asset URL, expected SHA-256, cache path, and offline policy.
# Outputs: Returns the verified cache path or throws before extraction.
function Get-ComparisonAsset {
    param([string]$Uri, [string]$Expected, [string]$Path, [bool]$CacheOnly)
    Assert-ComparisonOwnedPath -Path $Path -Boundary $outRoot
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

# Purpose: Provision the original timing driver without executing unverified distribution bytes.
# Inputs: Repository-owned tool root and the offline policy; immutable official asset and binary pins.
# Outputs: A verified Hyperfine installation and its exact pin evidence; stale or linked binaries fail closed.
function Initialize-Hyperfine {
    param([string]$Root, [bool]$CacheOnly)
    $directoryName = 'hyperfine-v1.20.0-x86_64-pc-windows-msvc'
    $destination = Join-Path $Root $directoryName
    $exe = Join-Path $destination 'hyperfine.exe'
    $binaryHash = 'FBAD9DFC44E98E47AAAF352F83F9C5273FC5008658A2F38FB5844ADC9FA9918B'
    $archiveHash = '2508C549B049B1D4342D08EDC1CB42BFAC169082B6E3069431B5BAB9822DBB32'
    Assert-ComparisonOwnedPath -Path $destination -Boundary $Root
    if (-not (Test-Path -LiteralPath $destination)) {
        $downloads = Join-Path $Root 'downloads'
        New-Item -ItemType Directory -Force -Path $downloads | Out-Null
        if ((Get-Item -LiteralPath $downloads).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing a linked Hyperfine download cache.'
        }
        $archive = Get-ComparisonAsset `
            -Uri "https://github.com/sharkdp/hyperfine/releases/download/v1.20.0/$directoryName.zip" `
            -Expected $archiveHash -Path (Join-Path $downloads "$directoryName.zip") -CacheOnly $CacheOnly
        $stage = Join-Path $Root ('hyperfine-stage-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        try {
            Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Archive/Microsoft.PowerShell.Archive.psd1') -ErrorAction Stop
            Expand-Archive -LiteralPath $archive -DestinationPath $stage
            $stagedDirectory = Join-Path $stage $directoryName
            Assert-ComparisonHash -Path (Join-Path $stagedDirectory 'hyperfine.exe') `
                -Expected $binaryHash -Label 'Staged Hyperfine executable'
            $sourceFull = [IO.Path]::GetFullPath($stagedDirectory)
            $destinationFull = [IO.Path]::GetFullPath($destination)
            $stagePrefix = [IO.Path]::GetFullPath($stage).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
            $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
            if (-not $sourceFull.StartsWith($stagePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                -not $destinationFull.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Refusing to publish an unverified Hyperfine installation path.'
            }
            [IO.Directory]::Move($sourceFull, $destinationFull)
        } finally {
            $resolvedStage = [IO.Path]::GetFullPath($stage)
            $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
            if (-not $resolvedStage.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Refusing to clean an unverified Hyperfine staging path.'
            }
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force
        }
    }
    if ((Get-Item -LiteralPath $exe).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing a linked Hyperfine executable.'
    }
    Assert-ComparisonHash -Path $exe -Expected $binaryHash -Label 'Hyperfine executable'
    [ordered]@{ tool = 'Hyperfine'; version = '1.20.0'; binary_sha256 = $binaryHash.ToLowerInvariant();
        archive_sha256 = $archiveHash.ToLowerInvariant() } | ConvertTo-Json -Compress
}

if ($Tool -eq 'Hyperfine') {
    Initialize-Hyperfine -Root $toolRoot -CacheOnly $Offline.IsPresent
    return
}

$sevenRoot = Join-Path $toolRoot '7zip-26.06'
$sevenFiles = @(
    @('x64/7za.exe', 'DD3757F31C97E33776F2590317D4FB12F2DA075DB3DBB1E3F94D0F2A01D84FFA'),
    @('x64/7za.dll', '2FCF9CEA76D55F16C8C9113B63FFB82C66E96BC679B22459436031C252916551'),
    @('x64/7zxa.dll', '25B19512A0482BCA7DB4B3E000F41EBE25BED40225DEE4CCB86417B7E35A4F57'),
    @('License.txt', '39D2187B942CC88818DD959E53C6B7DDDD152097A42061467C4D56050168D646')
)
$zstdDir = Join-Path $toolRoot 'zstd-v1.5.7-win64'
$zstdExeHash = '8076AAE03FEAC7C66B319579E82172EED168DEED2A3F25E5E2D3C60F55E84111'
$zstdLicense = Join-Path $repo 'third_party/zstd/LICENSE'
$zstdLicenseHash = '7055266497633C9025B777C78EB7235AF13922117480ED5C674677ADC381C9D8'
$zstdFiles = @(@('zstd.exe', $zstdExeHash), @('LICENSE', $zstdLicenseHash))
$sevenReady = Test-ComparisonInstallation -Root $sevenRoot -Files $sevenFiles -Label '7-Zip'
Restore-ComparisonNotice -Root $zstdDir -BinaryName 'zstd.exe' -BinaryHash $zstdExeHash `
    -Source $zstdLicense -NoticeName 'LICENSE' -NoticeHash $zstdLicenseHash
$zstdReady = Test-ComparisonInstallation -Root $zstdDir -Files $zstdFiles -Label 'Zstandard'
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
        $release = 'https://github.com/ip7z/7zip/releases/download/26.06/'
        $extractor = Get-ComparisonAsset -Uri ($release + '7zr.exe') `
            -Expected 'E80DB2DAD715A6C5ADABE292D31ED54E11F4E9B99CA349827B50573B175891BC' `
            -Path (Join-Path $cache '7zr-26.06.exe') -CacheOnly $Offline.IsPresent
        $archive = Get-ComparisonAsset -Uri ($release + '7z2606-extra.7z') `
            -Expected '396C180F12D2247C24B0B1FCD482C6FC2A2700F61211FFEDE1061DD971F5E104' `
            -Path (Join-Path $cache '7z2606-extra.7z') -CacheOnly $Offline.IsPresent
        $sevenStage = Join-Path $stage '7zip'
        New-Item -ItemType Directory -Path $sevenStage | Out-Null
        & $extractor x -y "-o$sevenStage" $archive 'x64/7za.exe' 'x64/7za.dll' 'x64/7zxa.dll' 'License.txt' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip asset extraction failed: $LASTEXITCODE" }
        if (-not (Test-ComparisonInstallation -Root $sevenStage -Files $sevenFiles -Label 'Staged 7-Zip')) {
            throw 'Staged 7-Zip installation is missing.'
        }
        if (Test-Path -LiteralPath $sevenRoot) { throw "Incomplete 7-Zip installation exists: $sevenRoot" }
        Move-Item -LiteralPath $sevenStage -Destination $sevenRoot
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
        Restore-ComparisonNotice -Root (Split-Path -Parent $stagedZstd) -BinaryName 'zstd.exe' -BinaryHash $zstdExeHash `
            -Source $zstdLicense -NoticeName 'LICENSE' -NoticeHash $zstdLicenseHash
        if (-not (Test-ComparisonInstallation -Root (Split-Path -Parent $stagedZstd) -Files $zstdFiles -Label 'Staged Zstandard')) {
            throw 'Staged Zstandard installation is missing.'
        }
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
