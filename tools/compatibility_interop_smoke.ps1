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

$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if (-not $tar) {
    $tar = Get-Command tar -ErrorAction SilentlyContinue
}
if (-not $tar) {
    throw "Windows tar/libarchive was not found; compatibility interop smoke cannot run."
}

# Purpose: Run a native command and fail with captured output on nonzero exit.
# Inputs: `FilePath` is the executable, `Arguments` are passed verbatim, and `Label` names the operation.
# Outputs: Returns combined output lines or throws with the native exit code.
function Invoke-InteropCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label
    )

    $output = & $FilePath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode. Output: $($output -join "`n")"
    }
    return @($output)
}

# Purpose: Run a native command that must fail with a specific diagnostic.
# Inputs: `FilePath` is the executable, `Arguments` are passed verbatim, `Label` names the operation, and `ExpectedText` is matched in stderr/stdout.
# Outputs: Returns normally only when the command fails and reports the expected diagnostic.
function Invoke-ExpectedInteropFailure {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$ExpectedText
    )

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $exitCode = $process.ExitCode
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
    if ($exitCode -eq 0) {
        throw "$Label unexpectedly succeeded. Output: $($output -join "`n")"
    }
    if (($output -join "`n") -notmatch [regex]::Escape($ExpectedText)) {
        throw "$Label failed without expected diagnostic '$ExpectedText'. Output: $($output -join "`n")"
    }
}

# Purpose: Create a deterministic text file for external-reader roundtrip checks.
# Inputs: `Path` is the output file and `Text` is the exact payload.
# Outputs: Creates parent directories and writes the payload.
function Write-InteropTextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    Set-Content -LiteralPath $Path -Value $Text -NoNewline
}

# Purpose: Assert that an independently extracted tree matches the source fixtures.
# Inputs: `SourceRoot` and `OutputRoot` are directory roots, and `RelativePath` lists files to compare.
# Outputs: Throws on missing files or SHA-256 mismatch.
function Assert-InteropRestoredPayload {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [string[]]$RelativePath = @("input\alpha.txt", "input\nested\beta.txt")
    )

    foreach ($relative in $RelativePath) {
        $source = Join-Path $SourceRoot $relative
        $restored = Join-Path $OutputRoot $relative
        if (-not (Test-Path -LiteralPath $restored)) {
            throw "External reader did not restore expected file: $relative"
        }
        $expected = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $restored).Hash
        if ($expected -ne $actual) {
            throw "External reader restored different bytes for $relative"
        }
    }
}

# Purpose: Verify that libarchive can list and extract a SuperZip-created compatibility archive.
# Inputs: `Archive`, `OutputRoot`, `SourceRoot`, and expected slash-form `Entries` define the check.
# Outputs: Throws on list, extraction, or byte-compare failure.
function Assert-LibarchiveInterop {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [string[]]$Entries = @("input/alpha.txt", "input/nested/beta.txt")
    )

    $listed = Invoke-InteropCommand -FilePath $tar.Source -Arguments @("-tf", $Archive) -Label "tar list $Archive"
    foreach ($entry in $Entries) {
        if ($listed -notcontains $entry) {
            throw "External libarchive listing for $Archive did not contain $entry. Listed: $($listed -join ', ')"
        }
    }
    New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
    Invoke-InteropCommand -FilePath $tar.Source -Arguments @("-xf", $Archive, "-C", $OutputRoot) -Label "tar extract $Archive" | Out-Null
    Assert-InteropRestoredPayload -SourceRoot $SourceRoot -OutputRoot $OutputRoot
}

# Purpose: Verify that PowerShell's ZIP reader can open a SuperZip-created ZIP archive.
# Inputs: `Archive`, `OutputRoot`, and `SourceRoot` define the check.
# Outputs: Extracts with `Expand-Archive` and throws on byte mismatch.
function Assert-PowerShellZipInterop {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$SourceRoot
    )

    New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
    Expand-Archive -LiteralPath $Archive -DestinationPath $OutputRoot -Force
    Assert-InteropRestoredPayload -SourceRoot $SourceRoot -OutputRoot $OutputRoot
}

$work = Join-Path $WorkRoot ("superzip-compat-interop-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $inputRoot = Join-Path $work "source"
    $inputTree = Join-Path $inputRoot "input"
    Write-InteropTextFile -Path (Join-Path $inputTree "alpha.txt") -Text "alpha interoperability payload"
    Write-InteropTextFile -Path (Join-Path $inputTree "nested\beta.txt") -Text "beta interoperability payload"

    $cases = @(
        @{ Format = "zip"; Extension = "zip"; PowerShellZip = $true; CompressionLevel = "9" },
        @{ Format = "tar"; Extension = "tar" },
        @{ Format = "tar.gz"; Extension = "tar.gz"; CompressionLevel = "9" },
        @{ Format = "tar.bz2"; Extension = "tar.bz2"; CompressionLevel = "9" },
        @{ Format = "tar.zst"; Extension = "tar.zst"; CompressionLevel = "9" },
        @{ Format = "cpio"; Extension = "cpio" },
        @{ Format = "cpio.gz"; Extension = "cpgz"; CompressionLevel = "9" },
        @{ Format = "ar"; Extension = "ar" }
    )

    foreach ($case in $cases) {
        $archive = Join-Path $work ("archive." + $case.Extension)
        $createArgs = @(
            "compress",
            "--format",
            [string]$case.Format
        )
        if ($case.ContainsKey("CompressionLevel")) {
            $createArgs += @("--compression-level", [string]$case.CompressionLevel)
        }
        $createArgs += @("--output", $archive, $inputTree)
        Invoke-InteropCommand -FilePath $cli -Arguments $createArgs -Label "SuperZip create $($case.Format)" | Out-Null
        if (-not (Test-Path -LiteralPath $archive)) {
            throw "SuperZip did not create expected compatibility archive: $archive"
        }
        if ($case.PowerShellZip) {
            Assert-PowerShellZipInterop -Archive $archive -OutputRoot (Join-Path $work "expanded-zip") -SourceRoot $inputRoot
        }
        Assert-LibarchiveInterop -Archive $archive -OutputRoot (Join-Path $work ("extract-" + $case.Extension)) -SourceRoot $inputRoot
        Write-Output "compat_interop format=$($case.Format) status=passed archive_bytes=$((Get-Item -LiteralPath $archive).Length)"
    }

    foreach ($format in @("tar", "cpio", "ar")) {
        $archive = Join-Path $work ("reject-level-" + $format + ".archive")
        Invoke-ExpectedInteropFailure -FilePath $cli -Arguments @(
            "compress",
            "--format",
            $format,
            "--compression-level",
            "9",
            "--output",
            $archive,
            $inputTree
        ) -Label "SuperZip reject compression level for $format" -ExpectedText "does not support compression-level flags"
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "Compatibility interoperability smoke passed using $($tar.Source)."
