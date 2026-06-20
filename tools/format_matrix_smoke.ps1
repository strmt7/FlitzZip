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

# Purpose: Run the SuperZip CLI and fail with captured output on nonzero exit.
# Inputs: `Arguments` are passed verbatim and `Label` names the operation.
# Outputs: Returns combined output lines or throws with the native exit code.
function Invoke-SuperZipMatrixCommand {
    param(
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label
    )

    $output = & $cli @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode. Output: $($output -join "`n")"
    }
    return @($output)
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

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process `
            -FilePath $cli `
            -ArgumentList $Arguments `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        $output = @()
        if (Test-Path -LiteralPath $stdoutPath) {
            $output += Get-Content -LiteralPath $stdoutPath
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $output += Get-Content -LiteralPath $stderrPath
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }

    if ($process.ExitCode -eq 0) {
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

# Purpose: Return the test file that exercises an extract-only format.
# Inputs: `Key` is a CLI archive format key.
# Outputs: Returns a repository-relative test path or `$null` when coverage is missing.
function Get-MatrixExtractOnlyTestPath {
    param([Parameter(Mandatory = $true)][string]$Key)

    $coverage = @{
        "zipx" = "tests\cpp\test_zip_compat.cpp"
        "7z" = "tests\cpp\test_sevenzip_compat.cpp"
        "tar.xz" = "tests\cpp\test_tar_xz_compat.cpp"
        "tar.lz" = "tests\cpp\test_lzip_compat.cpp"
        "b64" = "tests\cpp\test_base64_compat.cpp"
        "xz" = "tests\cpp\test_xz_compat.cpp"
        "lzma" = "tests\cpp\test_lzma_compat.cpp"
        "lz" = "tests\cpp\test_lzip_compat.cpp"
        "cab" = "tests\cpp\test_cab_compat.cpp"
        "iso" = "tests\cpp\test_iso_compat.cpp"
        "arj" = "tests\cpp\test_arj_compat.cpp"
        "arc" = "tests\cpp\test_arc_compat.cpp"
        "hqx" = "tests\cpp\test_hqx_compat.cpp"
        "macbinary" = "tests\cpp\test_macbinary_compat.cpp"
        "xxe" = "tests\cpp\test_xxe_compat.cpp"
        "uue" = "tests\cpp\test_uue_compat.cpp"
        "lha" = "tests\cpp\test_lha_compat.cpp"
        "wim" = "tests\cpp\test_wim_compat.cpp"
        "xar" = "tests\cpp\test_xar_compat.cpp"
        "deb" = "tests\cpp\test_ar_compat.cpp"
        "rpm" = "tests\cpp\test_rpm_compat.cpp"
    }
    return $coverage[$Key]
}

$work = Join-Path $WorkRoot ("superzip-format-matrix-" + [guid]::NewGuid().ToString("N"))
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
                foreach ($level in @("1", "9")) {
                    $levelArchive = Join-Path $archives ("level-$level-$token$extension")
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
                }
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
            $testPath = Get-MatrixExtractOnlyTestPath -Key $format.Key
            if (-not $testPath) {
                throw "Extract-only format $($format.Key) is missing matrix coverage mapping"
            }
            if (-not (Test-Path -LiteralPath (Join-Path $repo $testPath))) {
                throw "Extract-only format $($format.Key) coverage file is missing: $testPath"
            }
            Write-Output "format_matrix extract_only=$($format.Key) status=covered test=$testPath"
        }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "Format matrix smoke passed for $($formats.Count) registered formats."
