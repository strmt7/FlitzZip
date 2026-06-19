param(
    [string]$Configuration = "Release",
    [ValidateRange(1, 64)] [int]$SizeMiB = 8,
    [string]$WorkRoot = $env:TEMP
)

# Purpose: Prove that the required-GPU SUZIP filesystem path submits AMD HIP work and preserves data with a tiny bounded write workload.
# Inputs: `Configuration` selects the built CLI, `SizeMiB` controls the small temporary proof workload, and `WorkRoot` controls temporary storage.
# Outputs: Prints operation statistics and throws unless HIP kernel launches, HIP event time, transfer bytes, and restore hashes all validate.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}

# Purpose: Parse one stable `superzip_cli` key/value statistics line.
# Inputs: `Line` is one CLI output line containing `entries=... seconds=...`.
# Outputs: Returns a dictionary of parsed keys and values.
function ConvertFrom-StatsLine {
    param([Parameter(Mandatory = $true)][string]$Line)
    $result = @{}
    foreach ($part in ($Line -split "\s+")) {
        $pair = $part -split "=", 2
        if ($pair.Count -eq 2) {
            $result[$pair[0]] = $pair[1]
        }
    }
    if (-not $result.ContainsKey("seconds")) {
        throw "CLI did not emit an operation statistics line: $Line"
    }
    return $result
}

# Purpose: Return a numeric statistic from a parsed CLI stats dictionary.
# Inputs: `Stats` is a dictionary and `Key` is a metric name.
# Outputs: Returns the metric as a double or throws if the key is absent.
function Get-RequiredStatsNumber {
    param(
        [Parameter(Mandatory = $true)]$Stats,
        [Parameter(Mandatory = $true)][string]$Key
    )
    if (-not $Stats.ContainsKey($Key)) {
        throw "Required statistic missing: $Key"
    }
    return [double]$Stats[$Key]
}

# Purpose: Quote one Windows command-line argument for `ProcessStartInfo.Arguments`.
# Inputs: `Value` is one exact command-line argument.
# Outputs: Returns a string that the Windows C runtime parses back to the same argument.
function ConvertTo-WindowsArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($char in $Value.ToCharArray()) {
        if ($char -eq '\') {
            ++$slashes
            continue
        }
        if ($char -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes))
            $slashes = 0
        }
        [void]$builder.Append($char)
    }
    if ($slashes -gt 0) {
        [void]$builder.Append(('\' * ($slashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

# Purpose: Assert that one required-GPU operation submitted AMD HIP work.
# Inputs: `Stats` is a parsed CLI statistics dictionary and `Label` identifies the operation.
# Outputs: Throws if counters suggest CPU fallback, missing telemetry, or no HIP execution.
function Assert-GpuProofStat {
    param(
        [Parameter(Mandatory = $true)]$Stats,
        [Parameter(Mandatory = $true)][string]$Label,
        [bool]$RequireNativeCompressedBlocks = $false
    )
    if ($Stats["gpu_used"] -ne "true") {
        throw "$Label did not report gpu_used=true."
    }
    foreach ($key in @("gpu_kernel_launches", "gpu_kernel_ms", "gpu_h2d_bytes", "gpu_device_allocation_bytes")) {
        if ((Get-RequiredStatsNumber -Stats $Stats -Key $key) -le 0) {
            throw "$Label reported non-positive $key."
        }
    }
    $nativeCompressedBlocks =
        (Get-RequiredStatsNumber -Stats $Stats -Key "gpu_pattern_blocks") +
        (Get-RequiredStatsNumber -Stats $Stats -Key "gpu_prefix_blocks")
    if ($RequireNativeCompressedBlocks -and $nativeCompressedBlocks -le 0) {
        throw "$Label reported no GPU-compressed native blocks."
    }
}

# Purpose: Run one CLI operation and parse the statistics line.
# Inputs: `Arguments` is the exact CLI argument vector.
# Outputs: Prints CLI output and returns parsed statistics.
function Invoke-StatsCommand {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $output = & $cli @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "superzip_cli failed with exit code $LASTEXITCODE while running: $($Arguments -join ' ')"
    }
    $output | ForEach-Object { Write-Information -MessageData $_ -InformationAction Continue }
    $statsLine = $output | Where-Object { $_ -match '^entries=' } | Select-Object -Last 1
    return ConvertFrom-StatsLine -Line $statsLine
}

# Purpose: Run one CLI command that must fail.
# Inputs: `Arguments` is the exact CLI argument vector expected to return nonzero.
# Outputs: Returns normally only when the process fails as expected.
function Invoke-ExpectedFailure {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $cli
    $psi.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsArgument -Value $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -eq 0) {
        throw "Command unexpectedly succeeded: $($Arguments -join ' ')`n$stdout`n$stderr"
    }
}

# Purpose: Write deterministic benchmark-proof bytes without holding the whole workload in memory.
# Inputs: `Path` is the output file, `Bytes` is the target size, and `Kind` selects text-like or random content.
# Outputs: Creates a deterministic file for archive proofing.
function Write-ProofFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Bytes,
        [Parameter(Mandatory = $true)][ValidateSet("Text", "Random")] [string]$Kind
    )
    $buffer = New-Object byte[] (1MB)
    if ($Kind -eq "Text") {
        $line = [Text.Encoding]::UTF8.GetBytes("SuperZip AMD HIP proof line with repeated fields and deterministic payload.`n")
        for ($i = 0; $i -lt $buffer.Length; ++$i) {
            $buffer[$i] = $line[$i % $line.Length]
        }
    } else {
        $rng = [Random]::new(8675309)
    }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([int64]$buffer.Length, $remaining)
            if ($Kind -eq "Random") {
                $rng.NextBytes($buffer)
            }
            $stream.Write($buffer, 0, $count)
            $remaining -= $count
        }
    } finally {
        $stream.Dispose()
    }
}

& $cli dependency-check | ForEach-Object { Write-Information -MessageData $_ -InformationAction Continue }
if ($LASTEXITCODE -ne 0) {
    throw "GPU proof requires a HIP build with an available AMD GPU."
}

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$work = Join-Path $WorkRoot ("superzip-gpu-proof-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $sourceRoot = Join-Path $work "source"
    New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
    $textBytes = [int64]($SizeMiB * 1MB * 0.5)
    $randomBytes = ([int64]$SizeMiB * 1MB) - $textBytes
    Write-ProofFile -Path (Join-Path $sourceRoot "text.log") -Bytes $textBytes -Kind Text
    Write-ProofFile -Path (Join-Path $sourceRoot "random.bin") -Bytes $randomBytes -Kind Random

    $archive = Join-Path $work "proof.suzip"
    $archiveVerified = Join-Path $work "proof-verified.suzip"
    $invalidArchive = Join-Path $work "invalid.suzip"
    $outRoot = Join-Path $work "out"

    Invoke-ExpectedFailure -Arguments @("compress", "--format", "suzip", "--require-gpu", "--force-cpu", "--output", $invalidArchive, $sourceRoot)

    $compress = Invoke-StatsCommand -Arguments @("compress", "--format", "suzip", "--require-gpu", "--workers", "16", "--compression-level", "1", "--output", $archive, $sourceRoot)
    $compressVerified = Invoke-StatsCommand -Arguments @("compress", "--format", "suzip", "--require-gpu", "--workers", "16", "--compression-level", "1", "--verify-after-write", "--output", $archiveVerified, $sourceRoot)
    $verify = Invoke-StatsCommand -Arguments @("verify", "--require-gpu", "--workers", "16", $archive)
    $extract = Invoke-StatsCommand -Arguments @("extract", "--format", "suzip", "--require-gpu", "--workers", "16", "--overwrite", "--output", $outRoot, $archive)

    Assert-GpuProofStat -Stats $compress -Label "compress" -RequireNativeCompressedBlocks $true
    Assert-GpuProofStat -Stats $compressVerified -Label "compress --verify-after-write" -RequireNativeCompressedBlocks $true
    Assert-GpuProofStat -Stats $verify -Label "verify"
    Assert-GpuProofStat -Stats $extract -Label "extract"
    if ((Get-RequiredStatsNumber -Stats $compress -Key "output_bytes") -ge (Get-RequiredStatsNumber -Stats $compress -Key "input_bytes")) {
        throw "required-GPU compression did not reduce the proof workload size."
    }
    if ((Get-RequiredStatsNumber -Stats $verify -Key "gpu_d2h_bytes") -le 0) {
        throw "verify reported no GPU device-to-host bytes."
    }
    if ((Get-RequiredStatsNumber -Stats $extract -Key "gpu_d2h_bytes") -le 0) {
        throw "extract reported no GPU device-to-host bytes."
    }

    $sourceFiles = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File | Sort-Object FullName
    foreach ($file in $sourceFiles) {
        $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\')
        $restored = Join-Path (Join-Path $outRoot "source") $relative
        $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $restored).Hash
        if ($expectedHash -ne $actualHash) {
            throw "Restored file hash mismatch: $relative"
        }
    }
    Write-Information -MessageData "GPU proof passed: required-GPU commands submitted AMD HIP kernels and restored data correctly." -InformationAction Continue
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
