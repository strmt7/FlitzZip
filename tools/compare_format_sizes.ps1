param(
    [string]$Configuration = 'Release',
    [ValidateSet(4, 1024, 16384, 65536)][int[]]$SizeKiB = @(4, 1024),
    [ValidateSet('Text', 'SparseRecord', 'Incompressible')][string[]]$Profiles = @('Text', 'SparseRecord', 'Incompressible'),
    [ValidateSet(1, 5, 9)][int[]]$Levels = @(5),
    [ValidateSet(256, 512, 1024, 2048, 4096, 8192, 16384)][int]$NativeBlockSizeKiB = 8192,
    [string[]]$Formats = @(),
    [string]$OutputJson = '',
    [string]$SevenZipPath = ''
)

# Purpose: Compare exact, verified archive sizes across writable formats and independent open-source tools.
# Inputs: Bounded synthetic source shapes/sizes, product effort settings, optional format subset, and pinned tool paths.
# Outputs: Creates one new provenance JSON record; never reports filesystem timings as throughput evidence.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
$sevenZip = if ($SevenZipPath) { [IO.Path]::GetFullPath($SevenZipPath) } else {
    Join-Path $repo 'out\tools\7zip-26.03\x64\7za.exe'
}
$zstd = Join-Path $repo 'out\tools\zstd-v1.5.7-win64\zstd.exe'
$bsdtar = (Get-Command tar.exe -ErrorAction Stop).Source
foreach ($tool in @($cli, $sevenZip, $zstd, $bsdtar)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Comparison tool is unavailable: $tool. Run tools\bootstrap_comparison_tools.ps1 for pinned 7-Zip and Zstandard tools."
    }
}
$formatRows = @(
    [pscustomobject]@{ Key = 'suzip'; Extension = '.suzip'; LevelAware = $true }
    [pscustomobject]@{ Key = 'zip'; Extension = '.zip'; LevelAware = $true }
    [pscustomobject]@{ Key = 'tar'; Extension = '.tar'; LevelAware = $false }
    [pscustomobject]@{ Key = 'tar.gz'; Extension = '.tar.gz'; LevelAware = $true }
    [pscustomobject]@{ Key = 'tar.bz2'; Extension = '.tar.bz2'; LevelAware = $true }
    [pscustomobject]@{ Key = 'tar.zst'; Extension = '.tar.zst'; LevelAware = $true }
    [pscustomobject]@{ Key = 'gz'; Extension = '.gz'; LevelAware = $true }
    [pscustomobject]@{ Key = 'z'; Extension = '.Z'; LevelAware = $false }
    [pscustomobject]@{ Key = 'bz2'; Extension = '.bz2'; LevelAware = $true }
    [pscustomobject]@{ Key = 'zst'; Extension = '.zst'; LevelAware = $true }
    [pscustomobject]@{ Key = 'cpio'; Extension = '.cpio'; LevelAware = $false }
    [pscustomobject]@{ Key = 'cpio.gz'; Extension = '.cpio.gz'; LevelAware = $true }
    [pscustomobject]@{ Key = 'ar'; Extension = '.ar'; LevelAware = $false }
)

# Purpose: Run one native CLI without shell command construction or implicit success assumptions.
# Inputs: Absolute executable path, argument array, working directory, and diagnostic label.
# Outputs: Returns for exit zero; throws with bounded command output for any failure.
function Invoke-ComparisonTool {
    param([string]$Path, [string[]]$Arguments, [string]$Directory, [string]$Label)
    Push-Location -LiteralPath $Directory
    try {
        $output = @(& $Path @Arguments 2>&1)
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($code -ne 0) {
        $detail = (($output | Select-Object -Last 8) -join ' ').Substring(0, [Math]::Min(1000, (($output | Select-Object -Last 8) -join ' ').Length))
        throw "$Label failed with exit code $code. $detail"
    }
}

# Purpose: Fill one at-most-64-MiB source file with a reproducible, explicitly named data shape.
# Inputs: `Path`, byte `Count`, and `Profile` define the owned fixture.
# Outputs: Writes exactly `Count` bytes and returns its SHA-256 hash.
function Initialize-ComparisonSource {
    param([string]$Path, [int]$Count, [string]$FixtureProfile)
    $bytes = [byte[]]::new($Count)
    if ($FixtureProfile -eq 'Text') {
        $pattern = [Text.Encoding]::UTF8.GetBytes("SuperZip comparative text corpus. Deterministic line with markup, paths, numbers 0123456789.`n")
        for ($offset = 0; $offset -lt $Count; $offset += $pattern.Length) {
            [Array]::Copy($pattern, 0, $bytes, $offset, [Math]::Min($pattern.Length, $Count - $offset))
        }
    } elseif ($FixtureProfile -eq 'SparseRecord') {
        $motif = [byte[]]::new(16KB)
        [Random]::new(912817).NextBytes($motif)
        for ($offset = 0; $offset -lt $Count; $offset += $motif.Length) {
            [Array]::Copy($motif, 0, $bytes, $offset, [Math]::Min($motif.Length, $Count - $offset))
        }
        for ($offset = 1024; $offset -lt $Count; $offset += $motif.Length) {
            $bytes[$offset] = $bytes[$offset] -bxor [byte](1 + [int]($offset / $motif.Length) % 255)
        }
    } else {
        [Random]::new(912817).NextBytes($bytes)
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Purpose: Verify complete source recovery through SuperZip and an independent reader where one exists.
# Inputs: Source hash, archive path and format, destination, producing tool, and bounded workspace.
# Outputs: Returns a precise verification label or throws before recording a size.
function Assert-ComparisonArchive {
    param([string]$SourceHash, [string]$Archive, [string]$Format, [string]$Destination,
          [string]$Producer, [string]$Directory)
    New-Item -ItemType Directory -Path $Destination | Out-Null
    $extractArgs = @('extract', '--format', 'auto', '--output', $Destination)
    if ($Format -eq 'suzip') { $extractArgs += '--force-cpu' }
    Invoke-ComparisonTool -Path $cli -Arguments ($extractArgs + $Archive) -Directory $Directory -Label "$Producer $Format extraction"
    $files = @(Get-ChildItem -LiteralPath $Destination -Recurse -File)
    if ($files.Count -ne 1 -or (Get-FileHash -LiteralPath $files[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $SourceHash) {
        throw "$Producer $Format failed byte-exact SuperZip extraction."
    }
    if ($Producer -ne 'SuperZip') { return 'SuperZip independent extraction + SHA-256' }
    if ($Format -eq 'suzip') {
        Invoke-ComparisonTool -Path $cli -Arguments @('verify', '--force-cpu', $Archive) -Directory $Directory -Label 'native verification'
        return 'SuperZip extraction + SHA-256 + native verify'
    }
    if ($Format -in @('zip', 'gz', 'bz2')) {
        Invoke-ComparisonTool -Path $sevenZip -Arguments @('t', '-bd', '-bso0', '-bsp0', $Archive) -Directory $Directory -Label "7-Zip test $Format"
        return 'SuperZip extraction + SHA-256 + 7-Zip test'
    }
    if ($Format -eq 'zst') {
        Invoke-ComparisonTool -Path $zstd -Arguments @('-t', '-q', $Archive) -Directory $Directory -Label 'Zstandard test'
        return 'SuperZip extraction + SHA-256 + Zstandard test'
    }
    if ($Format -in @('tar', 'tar.gz', 'tar.bz2', 'tar.zst', 'cpio', 'cpio.gz', 'ar')) {
        Invoke-ComparisonTool -Path $bsdtar -Arguments @('-tf', $Archive) -Directory $Directory -Label "libarchive list $Format"
        return 'SuperZip extraction + SHA-256 + libarchive list'
    }
    return 'SuperZip extraction + SHA-256'
}

# Purpose: Read BZip2's encoded 100-KiB block-size selector for honest effort comparisons.
# Inputs: Verified archive path and its container format.
# Outputs: Returns the stream selector for BZip2 formats, or null for other formats.
function Get-ComparisonBzip2BlockSize {
    param([string]$Archive, [string]$Format)
    if ($Format -notin @('bz2', 'tar.bz2')) { return $null }
    $stream = [IO.File]::OpenRead($Archive)
    try {
        $header = [byte[]]::new(4)
        if ($stream.Read($header, 0, 4) -ne 4 -or
            [Text.Encoding]::ASCII.GetString($header, 0, 3) -ne 'BZh' -or
            $header[3] -lt [byte][char]'1' -or $header[3] -gt [byte][char]'9') {
            throw "Invalid BZip2 stream header in $Format comparison archive."
        }
        return [int]($header[3] - [byte][char]'0')
    } finally { $stream.Dispose() }
}

# Purpose: Measure one verified archive without treating a filesystem operation as a speed benchmark.
# Inputs: Producer, format, product/tool effort, native block size, source, case directory, and create command.
# Outputs: Returns exact encoded bytes, hashes, and verification provenance.
function Measure-ComparisonCase {
    param([string]$Producer, [string]$Format, [Nullable[int]]$Level, [Nullable[int]]$ToolLevel,
          [Nullable[int]]$NativeBlockSizeKiB, [string]$Source,
          [string]$SourceHash, [string]$Root, [string]$Extension, [string]$ToolPath,
          [string[]]$Arguments)
    $caseRoot = Join-Path $Root ('case-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $caseRoot | Out-Null
    $archive = Join-Path $caseRoot ("archive$Extension")
    $resolvedArguments = @($Arguments | ForEach-Object { if ($_ -eq '{archive}') { $archive } else { $_ } })
    Invoke-ComparisonTool -Path $ToolPath -Arguments $resolvedArguments -Directory (Split-Path -Parent $Source) -Label "$Producer $Format create"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) { throw "$Producer $Format did not create an archive." }
    $verification = Assert-ComparisonArchive -SourceHash $SourceHash -Archive $archive -Format $Format `
        -Destination (Join-Path $caseRoot 'extract') -Producer $Producer -Directory $caseRoot
    return [ordered]@{
        producer = $Producer
        format = $Format
        product_level = $Level
        tool_level = $ToolLevel
        native_block_size_kib = $NativeBlockSizeKiB
        effective_arguments = @($resolvedArguments | ForEach-Object {
            if ($_ -eq $archive) { '{archive}' }
            elseif ($_ -eq $Source) { '{source}' }
            else { $_ }
        })
        source_bytes = [int64](Get-Item -LiteralPath $Source).Length
        source_sha256 = $SourceHash
        archive_bytes = [int64](Get-Item -LiteralPath $archive).Length
        archive_sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        bzip2_block_size_100k = Get-ComparisonBzip2BlockSize -Archive $archive -Format $Format
        verification = $verification
        timing_scope = 'not_measured_filesystem_size_only'
    }
}

$registry = @(& $cli formats)
if ($LASTEXITCODE -ne 0) { throw 'SuperZip format registry query failed.' }
$writable = @($registry | Where-Object { $_ -match '^format=(\S+).*can_create=true' } |
    ForEach-Object { [regex]::Match($_, '^format=(\S+)').Groups[1].Value })
if (@(Compare-Object $writable @($formatRows.Key)).Count -ne 0) {
    throw 'Writable format registry changed; review comparison format/effort mapping before measuring.'
}
if ($Formats.Count -gt 0) {
    $unknown = @($Formats | Where-Object { $_ -notin $writable })
    if ($unknown.Count -gt 0) { throw "Unknown or extract-only comparison formats: $($unknown -join ', ')" }
    $formatRows = @($formatRows | Where-Object { $_.Key -in $Formats })
}
$nativeCasesPerFixture = 0
foreach ($format in $formatRows) {
    $nativeCasesPerFixture += if ($format.LevelAware) { $Levels.Count } else { 1 }
}
$externalCasesPerFixture = @('zip', 'gz', 'bz2', 'zst' |
    Where-Object { $_ -in $formatRows.Key }).Count * $Levels.Count
$externalCasesPerFixture += @('tar', 'tar', 'tar.gz', 'tar.bz2', 'tar.zst' |
    Where-Object { $_ -in $formatRows.Key }).Count
$casesPerFixture = $nativeCasesPerFixture + $externalCasesPerFixture
$plannedBytes = [int64](($SizeKiB | Measure-Object -Sum).Sum) * 1KB * $Profiles.Count *
    (1 + 4 * $casesPerFixture)
if ($plannedBytes -gt 512MB) {
    throw 'Comparison plan exceeds the 512 MiB conservative filesystem-write budget; select fewer cases.'
}
if (-not $OutputJson) { $OutputJson = Join-Path $repo ('out\benchmarks\format-sizes-' + [guid]::NewGuid().ToString('N') + '.json') }
$fullOutput = [IO.Path]::GetFullPath($OutputJson)
$benchmarkRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out\benchmarks'))
if (-not $fullOutput.StartsWith($benchmarkRoot + [IO.Path]::DirectorySeparatorChar,
                                [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Comparison JSON must be inside the repository ignored out\benchmarks directory.'
}
if (Test-Path -LiteralPath $fullOutput) { throw 'Comparison JSON already exists; choose a new path.' }
$outRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out\format-size-work'))
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
if ((Get-Item -LiteralPath $outRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Refusing a linked comparison workspace root.'
}
$work = Join-Path $outRoot ([guid]::NewGuid().ToString('N'))
$resolvedWork = [IO.Path]::GetFullPath($work)
if (-not $resolvedWork.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Comparison workspace escaped its owned output root.'
}
New-Item -ItemType Directory -Path $resolvedWork | Out-Null
$completed = $false
try {
    $sevenVersion = @(& $sevenZip i | Where-Object { $_ -match '^7-Zip' } | Select-Object -First 1)[0]
    $tarVersion = @(& $bsdtar --version | Select-Object -First 1)[0]
    $zstdVersion = @(& $zstd --version | Select-Object -First 1)[0]
    $tools = @(
        [ordered]@{ name = 'SuperZip'; version = 'source build'; sha256 = (Get-FileHash $cli -Algorithm SHA256).Hash.ToLowerInvariant() }
        [ordered]@{ name = '7-Zip'; version = $sevenVersion; sha256 = (Get-FileHash $sevenZip -Algorithm SHA256).Hash.ToLowerInvariant() }
        [ordered]@{ name = 'libarchive bsdtar'; version = $tarVersion; sha256 = (Get-FileHash $bsdtar -Algorithm SHA256).Hash.ToLowerInvariant() }
        [ordered]@{ name = 'Zstandard CLI'; version = $zstdVersion; sha256 = (Get-FileHash $zstd -Algorithm SHA256).Hash.ToLowerInvariant() }
    )
    $results = @()
    foreach ($size in $SizeKiB) {
        foreach ($fixtureProfile in $Profiles) {
            $sourceRoot = Join-Path $resolvedWork ("source-$size-$fixtureProfile")
            New-Item -ItemType Directory -Path $sourceRoot | Out-Null
            $source = Join-Path $sourceRoot 'source.bin'
            $sourceHash = Initialize-ComparisonSource -Path $source -Count ($size * 1KB) -FixtureProfile $fixtureProfile
            foreach ($format in $formatRows) {
                $levelsForFormat = @($Levels)
                if (-not $format.LevelAware) { $levelsForFormat = ,$null }
                foreach ($level in $levelsForFormat) {
                    $create = @('compress', '--format', $format.Key)
                    if ($format.Key -eq 'suzip') {
                        $create += @('--force-cpu', '--block-size-kib', "$NativeBlockSizeKiB")
                    }
                    if ($null -ne $level) { $create += @('--compression-level', "$level") }
                    $create += @('--output', '{archive}', $source)
                    $blockSize = if ($format.Key -eq 'suzip') { $NativeBlockSizeKiB } else { $null }
                    $result = Measure-ComparisonCase -Producer 'SuperZip' -Format $format.Key -Level $level `
                        -NativeBlockSizeKiB $blockSize `
                        -Source $source -SourceHash $sourceHash -Root $resolvedWork -Extension $format.Extension `
                        -ToolPath $cli -Arguments $create
                    $result['profile'] = $fixtureProfile
                    $results += $result
                }
            }
            foreach ($format in @('zip', 'tar', 'gz', 'bz2')) {
                if ($format -notin $formatRows.Key) { continue }
                $externalLevels = @($Levels)
                if ($format -eq 'tar') { $externalLevels = ,$null }
                foreach ($level in $externalLevels) {
                    $type = @{ zip = 'zip'; tar = 'tar'; gz = 'gzip'; bz2 = 'bzip2' }[$format]
                    $create = @('a', "-t$type", '-bd', '-bso0', '-bsp0')
                    if ($null -ne $level) { $create += "-mx=$level" }
                    $create += @('{archive}', 'source.bin')
                    $extension = ($formatRows | Where-Object Key -eq $format | Select-Object -First 1).Extension
                    $result = Measure-ComparisonCase -Producer '7-Zip' -Format $format -Level $null `
                        -ToolLevel $level -Source $source -SourceHash $sourceHash -Root $resolvedWork `
                        -Extension $extension -ToolPath $sevenZip -Arguments $create
                    $result['profile'] = $fixtureProfile
                    $results += $result
                }
            }
            foreach ($format in @('tar', 'tar.gz', 'tar.bz2', 'tar.zst')) {
                if ($format -notin $formatRows.Key) { continue }
                $filter = switch ($format) { 'tar.gz' { '-z' } 'tar.bz2' { '-j' } 'tar.zst' { '--zstd' } default { '' } }
                $create = @('-c', '--format=ustar')
                if ($filter) { $create += $filter }
                $create += @('-f', '{archive}', 'source.bin')
                $extension = ($formatRows | Where-Object Key -eq $format | Select-Object -First 1).Extension
                $result = Measure-ComparisonCase -Producer 'libarchive bsdtar' -Format $format -Level $null -ToolLevel $null `
                    -Source $source -SourceHash $sourceHash -Root $resolvedWork -Extension $extension `
                    -ToolPath $bsdtar -Arguments $create
                $result['profile'] = $fixtureProfile
                $results += $result
            }
            if ('zst' -in $formatRows.Key) {
                foreach ($level in $Levels) {
                    $mapped = @{ 1 = 1; 5 = 5; 9 = 22 }[$level]
                    $create = @('-q', '-f')
                    if ($mapped -gt 19) { $create += '--ultra' }
                    $create += @("-$mapped", 'source.bin', '-o', '{archive}')
                    $result = Measure-ComparisonCase -Producer 'Zstandard CLI' -Format 'zst' -Level $null `
                        -ToolLevel $mapped -Source $source -SourceHash $sourceHash -Root $resolvedWork `
                        -Extension '.zst' -ToolPath $zstd -Arguments $create
                    $result['profile'] = $fixtureProfile
                    $results += $result
                }
            }
        }
    }
    $expectedCases = $SizeKiB.Count * $Profiles.Count * $casesPerFixture
    if ($results.Count -ne $expectedCases) {
        throw "Comparison row coverage incomplete: expected $expectedCases, observed $($results.Count)."
    }
    $commit = (& git -C $repo rev-parse HEAD).Trim()
    $dirty = @(& git -C $repo status --porcelain --untracked-files=normal).Count -gt 0
    $record = [ordered]@{
        schema_version = 1
        benchmark_kind = 'format_size_matrix'
        recorded_utc = (Get-Date).ToUniversalTime().ToString('o')
        source_commit = $commit
        source_dirty = $dirty
        source_generator = 'compare_format_sizes.ps1/v1; deterministic .NET Random seed 912817'
        tools = $tools
        cases = $results
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $fullOutput) | Out-Null
    $stream = [IO.FileStream]::new($fullOutput, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        try { $writer.WriteLine(($record | ConvertTo-Json -Depth 7)) } finally { $writer.Dispose() }
    } finally { $stream.Dispose() }
    Write-Output "format_size_matrix status=passed cases=$($results.Count) source_dirty=$dirty output=$fullOutput"
    $completed = $true
} finally {
    if ($completed) {
        $safeRoot = [IO.Path]::GetFullPath($outRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $resolvedWork.StartsWith($safeRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to remove an unverified comparison workspace.'
        }
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    } else {
        Write-Warning "Comparison workspace retained after failure: $resolvedWork"
    }
}
