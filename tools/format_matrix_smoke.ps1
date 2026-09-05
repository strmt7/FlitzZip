param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$WorkRoot = $env:TEMP
)

$ErrorActionPreference = "Stop"
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path -LiteralPath $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}
$testRunner = Join-Path $repo "build\$Configuration\superzip_tests.exe"
if (-not (Test-Path -LiteralPath $testRunner)) {
    throw "Native fixture test binary not found. Run tools/build.ps1 first."
}

# Purpose: Preserve one exact argument through the Windows CRT command-line parser.
# Inputs: Value is a possibly empty argument, including spaces, quotes, or trailing backslashes.
# Outputs: Returns a correctly quoted ProcessStartInfo.Arguments component.
function ConvertTo-MatrixArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($char in $Value.ToCharArray()) {
        if ($char -eq '\') { ++$slashes; continue }
        if ($char -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
        } elseif ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes))
        }
        [void]$builder.Append($char)
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

# Purpose: Execute one matrix command without leaking its native exit code to the host shell.
# Inputs: FilePath/Arguments select the executable; FixtureRoot optionally requests native fixture export.
# Outputs: Returns exit code and captured lines; throws on launch failure or the two-minute deadline.
function Invoke-MatrixProcess {
    param([string]$FilePath, [string[]]$Arguments, [string]$FixtureRoot = "")

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.Arguments = (($Arguments | ForEach-Object { ConvertTo-MatrixArgument -Value $_ }) -join ' ')
    $start.WorkingDirectory = $repo
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables.Remove('SUPERZIP_TEST_FIXTURE_EXPORT')
    if ($FixtureRoot) { $start.EnvironmentVariables['SUPERZIP_TEST_FIXTURE_EXPORT'] = $FixtureRoot }
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(120000)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw "Matrix command timed out: $FilePath"
        }
        $output = $stdout.GetAwaiter().GetResult() + "`n" + $stderr.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = @($output -split '\r?\n' | Where-Object { $_.Length -gt 0 })
        }
    } finally {
        $process.Dispose()
    }
}

# Purpose: Run the SuperZip CLI and fail with captured output on nonzero exit.
# Inputs: `Arguments` are passed verbatim and `Label` names the operation.
# Outputs: Returns combined output lines or throws with the native exit code.
function Invoke-SuperZipMatrixCommand {
    param(
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label
    )

    $result = Invoke-MatrixProcess -FilePath $cli -Arguments $Arguments
    if ($result.ExitCode -ne 0) {
        throw "$Label failed with exit code $($result.ExitCode). Output: $($result.Output -join "`n")"
    }
    return @($result.Output)
}

# Purpose: Run a SuperZip CLI command that must fail with a specific diagnostic fragment.
# Inputs: `Arguments` are passed verbatim, `Label` names the operation, and `ExpectedText` is matched in output.
# Outputs: Returns normally only when the command fails and reports the expected text.
function Invoke-ExpectedMatrixFailure {
    param(
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$ExpectedText
    )

    $result = Invoke-MatrixProcess -FilePath $cli -Arguments $Arguments
    $output = $result.Output
    if ($result.ExitCode -eq 0) {
        throw "$Label unexpectedly succeeded. Output: $($output -join "`n")"
    }
    if (($output -join "`n") -notmatch [regex]::Escape($ExpectedText)) {
        throw "$Label failed without expected diagnostic '$ExpectedText'. Output: $($output -join "`n")"
    }
}

# Purpose: Parse one `superzip_cli formats` line into structured metadata.
# Inputs: `Line` is one machine-readable registry record.
# Outputs: Returns a format object or throws when the CLI contract changes.
function ConvertFrom-FormatRegistryLine {
    param([Parameter(Mandatory = $true)][string]$Line)

    $pattern = '^format=(?<key>\S+) display="(?<display>[^"]*)" extensions="(?<extensions>[^"]*)" can_create=(?<create>true|false) can_extract=(?<extract>true|false) gpu_native=(?<gpu>true|false) bundled_native=(?<bundled>true|false)$'
    if ($Line -notmatch $pattern) {
        throw "Unexpected format registry line: $Line"
    }
    [pscustomobject]@{
        Key = $Matches.key
        Display = $Matches.display
        Extensions = $Matches.extensions
        CanCreate = ($Matches.create -eq "true")
        CanExtract = ($Matches.extract -eq "true")
        GpuNative = ($Matches.gpu -eq "true")
        BundledNative = ($Matches.bundled -eq "true")
    }
}

# Purpose: Create deterministic file and tree fixtures for archive roundtrip checks.
# Inputs: `Root` is the temporary workspace root.
# Outputs: Returns paths for a nested source tree and single-file source directory.
function Initialize-FormatMatrixFixture {
    param([Parameter(Mandatory = $true)][string]$Root)

    $tree = Join-Path $Root "matrix-tree"
    New-Item -ItemType Directory -Force -Path (Join-Path $tree "nested\empty-dir") | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $tree "alpha.txt"), "alpha archive matrix payload`n")
    [System.IO.File]::WriteAllText((Join-Path $tree "nested\beta file.txt"), "beta archive matrix payload`n")
    [System.IO.File]::WriteAllBytes((Join-Path $tree "empty.bin"), [byte[]]::new(0))
    $binary = [byte[]]::new(16384)
    for ($i = 0; $i -lt $binary.Length; $i += 1) {
        $binary[$i] = [byte](($i * 37 + [Math]::Floor($i / 11)) -band 0xFF)
    }
    [System.IO.File]::WriteAllBytes((Join-Path $tree "nested\mixed.bin"), $binary)

    $singleRoot = Join-Path $Root "single"
    New-Item -ItemType Directory -Force -Path $singleRoot | Out-Null
    [pscustomobject]@{
        Tree = $tree
        SingleRoot = $singleRoot
    }
}

# Purpose: Return a stable relative path for a filesystem item below a root.
# Inputs: `Root` is the base directory and `Path` is a contained item path.
# Outputs: Returns a backslash-normalized relative path.
function ConvertTo-MatrixRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $prefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    $full = [System.IO.Path]::GetFullPath($Path)
    return $full.Substring($prefix.Length).TrimStart('\')
}

# Purpose: List relative paths of directories or regular files below a root.
# Inputs: `Root` is the directory and `Kind` selects `Directory` or `File`.
# Outputs: Returns sorted relative paths.
function Get-MatrixRelativeItem {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [ValidateSet("Directory", "File")][string]$Kind = "File"
    )

    if (-not (Test-Path -LiteralPath $Root)) {
        return @()
    }
    $items = Get-ChildItem -LiteralPath $Root -Recurse -Force |
        Where-Object { if ($Kind -eq "Directory") { $_.PSIsContainer } else { -not $_.PSIsContainer } } |
        ForEach-Object { ConvertTo-MatrixRelativePath -Root $Root -Path $_.FullName } |
        Sort-Object
    return @($items)
}

# Purpose: Verify two directory trees have identical directories, files, and file bytes.
# Inputs: `ExpectedRoot` and `ActualRoot` are directory trees to compare.
# Outputs: Throws on missing paths, extra paths, or SHA-256 mismatch.
function Test-MatrixDirectoryMatch {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedRoot,
        [Parameter(Mandatory = $true)][string]$ActualRoot,
        [bool]$CompareDirectories = $true
    )

    $kinds = if ($CompareDirectories) { @("Directory", "File") } else { @("File") }
    foreach ($kind in $kinds) {
        $expected = @(Get-MatrixRelativeItem -Root $ExpectedRoot -Kind $kind)
        $actual = @(Get-MatrixRelativeItem -Root $ActualRoot -Kind $kind)
        $expectedText = $expected -join "`n"
        $actualText = $actual -join "`n"
        if ($expectedText -ne $actualText) {
            throw "Directory comparison failed for $kind paths. Expected: $expectedText Actual: $actualText"
        }
    }

    foreach ($relative in Get-MatrixRelativeItem -Root $ExpectedRoot -Kind File) {
        $expectedPath = Join-Path $ExpectedRoot $relative
        $actualPath = Join-Path $ActualRoot $relative
        $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $expectedPath).Hash
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $actualPath).Hash
        if ($expectedHash -ne $actualHash) {
            throw "File bytes differ for $relative"
        }
    }
}

# Purpose: Verify two regular files have identical bytes.
# Inputs: `ExpectedFile` and `ActualFile` are files to hash.
# Outputs: Throws on missing files or SHA-256 mismatch.
function Test-MatrixFileMatch {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedFile,
        [Parameter(Mandatory = $true)][string]$ActualFile
    )

    if (-not (Test-Path -LiteralPath $ActualFile -PathType Leaf)) {
        throw "Expected extracted file was not created: $ActualFile"
    }
    $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ExpectedFile).Hash
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ActualFile).Hash
    if ($expectedHash -ne $actualHash) {
        throw "File bytes differ for $ActualFile"
    }
}

# Purpose: Build a safe filesystem token from a format key.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns a filename-safe token.
function ConvertTo-MatrixToken {
    param([Parameter(Mandatory = $true)][string]$Key)

    return ($Key -replace '[^A-Za-z0-9]+', '-').Trim('-')
}

# Purpose: Return whether a create format accepts compression-level flags.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns true for level-aware encoders.
function Test-MatrixLevelAwareFormat {
    param([Parameter(Mandatory = $true)][string]$Key)

    return @("suzip", "zip", "tar.gz", "tar.bz2", "tar.zst", "gz", "bz2", "zst", "cpio.gz") -contains $Key
}

# Purpose: Return whether a create format requires exactly one regular-file source.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns true for single-stream encoders.
function Test-MatrixSingleFileFormat {
    param([Parameter(Mandatory = $true)][string]$Key)

    return @("gz", "bz2", "zst", "z") -contains $Key
}

# Purpose: Return whether a format promises empty-directory preservation in this smoke fixture.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns false for regular-file-only containers such as Unix AR.
function Test-MatrixDirectoryPreservingFormat {
    param([Parameter(Mandatory = $true)][string]$Key)

    return $Key -ne "ar"
}

# Purpose: Create one source file whose name matches the archive-derived extraction name.
# Inputs: `SingleRoot`, `Token`, and `Extension` identify the path.
# Outputs: Returns the single source file path.
function Initialize-MatrixSingleSource {
    param(
        [Parameter(Mandatory = $true)][string]$SingleRoot,
        [Parameter(Mandatory = $true)][string]$Token,
        [Parameter(Mandatory = $true)][string]$Extension
    )

    $baseName = "payload-$Token.bin"
    $source = Join-Path $SingleRoot $baseName
    $payload = [byte[]]::new(32768)
    for ($i = 0; $i -lt $payload.Length; $i += 1) {
        $payload[$i] = [byte](($i * 19 + $Token.Length + [Math]::Floor($i / 7)) -band 0xFF)
    }
    [System.IO.File]::WriteAllBytes($source, $payload)
    [pscustomobject]@{
        Source = $source
        ArchiveName = "$baseName$Extension"
        ExtractedName = $baseName
    }
}

# Purpose: Parse the `format=` value from `superzip_cli identify` output.
# Inputs: `Archive` is the archive path to identify.
# Outputs: Returns the detected format key.
function Get-MatrixIdentifiedFormat {
    param([Parameter(Mandatory = $true)][string]$Archive)

    $identified = Invoke-SuperZipMatrixCommand -Arguments @("identify", $Archive) -Label "identify $Archive"
    if (($identified -join "`n") -notmatch 'format=(?<key>\S+)') {
        throw "Could not parse identify output for $Archive. Output: $($identified -join "`n")"
    }
    return $Matches.key
}

# Purpose: Return a passing-fixture test for an extract-only format.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns the exact native test name or `$null` when coverage is missing.
function Get-MatrixExtractOnlyTestName {
    param([Parameter(Mandatory = $true)][string]$Key)

    $coverage = @{
        "zipx" = "zipx_extracts_zip_compatible_records"
        "7z" = "sevenzip_extraction_reads_nested_payload"
        "tar.xz" = "tar_xz_extracts_files_and_directories"
        "tar.lz" = "tar_lzip_extracts_files_and_directories"
        "b64" = "base64_compat_roundtrip"
        "xz" = "xz_extracts_single_file_fixture"
        "lzma" = "lzma_extracts_single_file_fixture"
        "lz" = "lzip_extracts_single_file_fixture"
        "cab" = "cab_extraction_reads_uncompressed_payload"
        "iso" = "iso_extraction_reads_basic_iso9660_files_and_directories"
        "arj" = "arj_extracts_stored_files_and_directories"
        "arc" = "arc_extracts_unpacked_files"
        "hqx" = "hqx_extracts_data_fork_and_discards_resource_fork"
        "macbinary" = "macbinary_extracts_data_fork_and_discards_resource_fork"
        "xxe" = "xxe_compat_roundtrip"
        "uue" = "uue_compat_roundtrip"
        "lha" = "lha_extraction_reads_nested_payload"
        "wim" = "wim_fixture_extracts_with_native_adapter"
        "xar" = "xar_extraction_reads_nested_zlib_payload"
        "deb" = "deb_outer_container_extracts_with_native_ar_adapter"
        "rpm" = "rpm_extraction_reads_gzip_cpio_payload"
    }
    return $coverage[$Key]
}

# Purpose: Prove an extract-only format through its fixture test and public CLI paths.
# Inputs: Format is one registry row; Work is the caller-owned temporary matrix directory.
# Outputs: Throws on missing/failed fixtures, identification, byte/tree mismatch, or overwrite behavior.
function Test-MatrixExtractOnlyFormat {
    param([Parameter(Mandatory = $true)]$Format, [string]$Work)

    $name = Get-MatrixExtractOnlyTestName -Key $Format.Key
    if (-not $name) { throw "Extract-only format $($Format.Key) is missing a fixture test mapping." }
    $token = ConvertTo-MatrixToken -Key $Format.Key
    $export = Join-Path $Work "fixture-$token"
    $result = Invoke-MatrixProcess -FilePath $testRunner -Arguments @($name) -FixtureRoot $export
    if ($result.ExitCode -ne 0 -or $result.Output -cnotcontains "[PASS] $name" -or
        $result.Output -cnotcontains '1 tests, 0 failed') {
        throw "Fixture test $name did not pass exactly once. Output: $($result.Output -join "`n")"
    }
    $archiveRoot = Join-Path $export "archive"
    $expected = Join-Path $export "expected"
    $archives = @(Get-ChildItem -LiteralPath $archiveRoot -File)
    if ($archives.Count -ne 1 -or -not (Test-Path -LiteralPath $expected -PathType Container)) {
        throw "Fixture test $name did not export one archive and its verified output tree. Rebuild the test binary."
    }
    $archive = $archives[0].FullName
    if ((Get-MatrixIdentifiedFormat -Archive $archive) -ne $Format.Key) {
        throw "CLI identification disagrees with the $($Format.Key) fixture."
    }
    foreach ($mode in @('auto', $Format.Key)) {
        $output = Join-Path $export "extract-$mode"
        $arguments = @('extract', '--format', $mode, '--output', $output, $archive)
        Invoke-SuperZipMatrixCommand -Arguments $arguments -Label "fixture extract $($Format.Key) mode=$mode" | Out-Null
        Test-MatrixDirectoryMatch -ExpectedRoot $expected -ActualRoot $output
        Invoke-ExpectedMatrixFailure -Arguments $arguments -Label "fixture overwrite refusal $($Format.Key)" -ExpectedText 'refusing to overwrite'
        Test-MatrixDirectoryMatch -ExpectedRoot $expected -ActualRoot $output
        $overwrite = @('extract', '--format', $mode, '--overwrite', '--output', $output, $archive)
        Invoke-SuperZipMatrixCommand -Arguments $overwrite -Label "fixture overwrite $($Format.Key) mode=$mode" | Out-Null
        Test-MatrixDirectoryMatch -ExpectedRoot $expected -ActualRoot $output
    }
    $sourceExtension = @($Format.Extensions -split ',' |
        Where-Object { $archive.EndsWith($_, [System.StringComparison]::OrdinalIgnoreCase) } |
        Sort-Object -Property Length -Descending | Select-Object -First 1)
    # A header-detected fixture (for example MacBinary .bin) need not use an advertised alias.
    $suffix = if ($sourceExtension.Count -eq 1) { $sourceExtension[0] } else { [System.IO.Path]::GetExtension($archive) }
    $baseName = $archives[0].Name.Substring(0, $archives[0].Name.Length - $suffix.Length)
    foreach ($alias in ($Format.Extensions -split ',')) {
        $aliasRoot = Join-Path $export ("alias" + $alias)
        New-Item -ItemType Directory -Path $aliasRoot | Out-Null
        $aliasArchive = Join-Path $aliasRoot ("$baseName$alias")
        Copy-Item -LiteralPath $archive -Destination $aliasArchive
        if ((Get-MatrixIdentifiedFormat -Archive $aliasArchive) -ne $Format.Key) {
            throw "Alias $alias did not identify as $($Format.Key)."
        }
        $output = Join-Path $export ("alias-output" + $alias)
        Invoke-SuperZipMatrixCommand -Arguments @('extract', '--format', 'auto', '--output', $output, $aliasArchive) -Label "fixture alias $alias" | Out-Null
        Test-MatrixDirectoryMatch -ExpectedRoot $expected -ActualRoot $output
    }
    Write-Output "format_matrix extract_only=$($Format.Key) status=passed verification=cli_fixture test=$name"
}

$rootPrefix = [System.IO.Path]::GetFullPath($WorkRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$work = [System.IO.Path]::GetFullPath((Join-Path $rootPrefix ("superzip-format-matrix-" + [guid]::NewGuid().ToString("N"))))
if (-not $work.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Matrix temporary directory escaped the requested workspace."
}
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $fixtures = Initialize-FormatMatrixFixture -Root (Join-Path $work "fixtures")
    $archives = Join-Path $work "archives"
    New-Item -ItemType Directory -Force -Path $archives | Out-Null
    $formats = @(Invoke-SuperZipMatrixCommand -Arguments @("formats") -Label "format registry" |
        ForEach-Object { ConvertFrom-FormatRegistryLine -Line $_ })

    foreach ($format in $formats) {
        $token = ConvertTo-MatrixToken -Key $format.Key
        $extension = (($format.Extensions -split ",")[0]).Trim()
        if ($format.CanCreate) {
            $single = Test-MatrixSingleFileFormat -Key $format.Key
            if ($single) {
                $singleSource = Initialize-MatrixSingleSource -SingleRoot $fixtures.SingleRoot -Token $token -Extension $extension
                $source = $singleSource.Source
                $archive = Join-Path $archives $singleSource.ArchiveName
                $expectedOutput = Join-Path (Join-Path $work "extract-$token") $singleSource.ExtractedName
            } else {
                $source = $fixtures.Tree
                $archive = Join-Path $archives ("archive-$token$extension")
                $expectedOutput = Join-Path (Join-Path $work "extract-$token") (Split-Path -Leaf $fixtures.Tree)
            }

            $createArgs = @("compress", "--format", $format.Key)
            if ($format.Key -eq "suzip") {
                $createArgs += @("--force-cpu", "--verify-after-write")
            }
            if (Test-MatrixLevelAwareFormat -Key $format.Key) {
                $createArgs += @("--compression-level", "5")
            }
            $createArgs += @("--output", $archive, $source)
            Invoke-SuperZipMatrixCommand -Arguments $createArgs -Label "create $($format.Key)" | Out-Null

            $detected = Get-MatrixIdentifiedFormat -Archive $archive
            if ($detected -ne $format.Key) {
                throw "Identify detected $detected for $archive; expected $($format.Key)"
            }

            $extractRoot = Split-Path -Parent $expectedOutput
            $extractArgs = @("extract", "--format", "auto", "--output", $extractRoot)
            if ($format.Key -eq "suzip") {
                $extractArgs += @("--force-cpu")
            }
            $extractArgs += @($archive)
            Invoke-SuperZipMatrixCommand -Arguments $extractArgs -Label "extract $($format.Key)" | Out-Null
            if ($single) {
                Test-MatrixFileMatch -ExpectedFile $source -ActualFile $expectedOutput
            } else {
                Test-MatrixDirectoryMatch `
                    -ExpectedRoot $source `
                    -ActualRoot $expectedOutput `
                    -CompareDirectories (Test-MatrixDirectoryPreservingFormat -Key $format.Key)
            }

            Invoke-ExpectedMatrixFailure `
                -Arguments $extractArgs `
                -Label "overwrite refusal $($format.Key)" `
                -ExpectedText "refusing to overwrite"
            $overwriteArgs = @("extract", "--format", "auto", "--overwrite", "--output", $extractRoot)
            if ($format.Key -eq "suzip") {
                $overwriteArgs += @("--force-cpu")
            }
            $overwriteArgs += @($archive)
            Invoke-SuperZipMatrixCommand -Arguments $overwriteArgs -Label "overwrite extract $($format.Key)" | Out-Null

            if ($format.Key -eq "suzip") {
                Invoke-SuperZipMatrixCommand -Arguments @("verify", "--force-cpu", $archive) -Label "verify suzip" | Out-Null
            }

            $aliases = @($format.Extensions -split "," | Select-Object -Skip 1)
            foreach ($alias in $aliases) {
                $aliasArchive = Join-Path $archives ("alias-$token$alias")
                Copy-Item -LiteralPath $archive -Destination $aliasArchive -Force
                $aliasDetected = Get-MatrixIdentifiedFormat -Archive $aliasArchive
                if ($aliasDetected -ne $format.Key) {
                    throw "Alias $alias detected $aliasDetected; expected $($format.Key)"
                }
            }

            if ($single) {
                $second = Join-Path $fixtures.SingleRoot "second-source.bin"
                [System.IO.File]::WriteAllText($second, "second")
                Invoke-ExpectedMatrixFailure `
                    -Arguments @("compress", "--format", $format.Key, "--output", (Join-Path $archives "multi-$token$extension"), $source, $second) `
                    -Label "single-source rejection $($format.Key)" `
                    -ExpectedText "requires exactly one regular-file source"
            }

            if (-not (Test-MatrixLevelAwareFormat -Key $format.Key)) {
                Invoke-ExpectedMatrixFailure `
                    -Arguments @("compress", "--format", $format.Key, "--compression-level", "9", "--output", (Join-Path $archives "level-$token$extension"), $source) `
                    -Label "compression-level rejection $($format.Key)" `
                    -ExpectedText "does not support compression-level flags"
            } else {
                foreach ($level in @("1", "2", "3", "4", "6", "7", "8", "9")) {
                    $levelRoot = Join-Path $work "level-$level-$token"
                    New-Item -ItemType Directory -Path $levelRoot | Out-Null
                    $levelArchive = Join-Path $levelRoot (Split-Path -Leaf $archive)
                    $levelArgs = @("compress", "--format", $format.Key)
                    if ($format.Key -eq "suzip") {
                        $levelArgs += @("--force-cpu")
                    }
                    $levelArgs += @("--compression-level", $level, "--output", $levelArchive, $source)
                    Invoke-SuperZipMatrixCommand -Arguments $levelArgs -Label "level $level create $($format.Key)" | Out-Null
                    $levelDetected = Get-MatrixIdentifiedFormat -Archive $levelArchive
                    if ($levelDetected -ne $format.Key) {
                        throw "Level $level archive detected $levelDetected; expected $($format.Key)"
                    }
                    $levelOutput = Join-Path $levelRoot "output"
                    $levelExtract = @("extract", "--format", "auto", "--output", $levelOutput)
                    if ($format.Key -eq "suzip") { $levelExtract += "--force-cpu" }
                    $levelExtract += $levelArchive
                    Invoke-SuperZipMatrixCommand -Arguments $levelExtract -Label "level $level extract $($format.Key)" | Out-Null
                    $levelExpected = Join-Path $levelOutput (Split-Path -Leaf $source)
                    if ($single) {
                        Test-MatrixFileMatch -ExpectedFile $source -ActualFile $levelExpected
                    } else {
                        Test-MatrixDirectoryMatch -ExpectedRoot $source -ActualRoot $levelExpected `
                            -CompareDirectories (Test-MatrixDirectoryPreservingFormat -Key $format.Key)
                    }
                }
                Write-Output "format_matrix compression_levels=$($format.Key) levels=1-9 status=passed verification=cli_roundtrip"
            }

            Write-Output "format_matrix create_extract=$($format.Key) status=passed archive_bytes=$((Get-Item -LiteralPath $archive).Length)"
        } else {
            Invoke-ExpectedMatrixFailure `
                -Arguments @("compress", "--format", $format.Key, "--output", (Join-Path $archives "unsupported-$token.bin"), $fixtures.Tree) `
                -Label "unsupported create $($format.Key)" `
                -ExpectedText "recognized but not yet implemented for create"
        }

        if (-not $format.CanExtract) {
            $dummy = Join-Path $archives ("unsupported-$token$extension")
            [System.IO.File]::WriteAllText($dummy, "unsupported")
            Invoke-ExpectedMatrixFailure `
                -Arguments @("extract", "--format", $format.Key, "--output", (Join-Path $work "unsupported-extract-$token"), $dummy) `
                -Label "unsupported extract $($format.Key)" `
                -ExpectedText "recognized but not yet implemented for extract"
        } elseif (-not $format.CanCreate) {
            Test-MatrixExtractOnlyFormat -Format $format -Work $work
        }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "Format matrix smoke passed for $($formats.Count) registered formats."
