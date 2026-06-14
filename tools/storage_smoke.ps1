param(
    [string]$Configuration = "Release",
    [ValidateRange(1, 64)] [int]$SizeMiB = 8,
    [string]$WorkRoot = $env:TEMP,
    [switch]$ForceCpu
)

# Purpose: Smoke-test the archive filesystem path with a tiny bounded write workload.
# Inputs: `Configuration` selects the built CLI, `SizeMiB` limits generated storage writes, `WorkRoot` selects the temporary location, and `ForceCpu` disables required HIP for diagnostics.
# Outputs: Verifies compress/verify/extract correctness and deletes temporary files; throws on any failure.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}

# Purpose: Write a deterministic small payload without allocating the whole file twice.
# Inputs: `Path` receives the file and `Bytes` is the exact length.
# Outputs: Creates or overwrites the file with deterministic bytes.
function Write-SmokePayload {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Bytes
    )
    $buffer = New-Object byte[] (1MB)
    for ($i = 0; $i -lt $buffer.Length; ++$i) {
        $buffer[$i] = [byte](($i * 131 + 17) -band 0xFF)
    }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([int64]$buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $remaining -= $count
        }
    } finally {
        $stream.Dispose()
    }
}

$modeFlag = if ($ForceCpu) { "--force-cpu" } else { "--require-gpu" }
if (-not $ForceCpu) {
    & $cli dependency-check | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Storage smoke requires a HIP build with an available AMD GPU. Pass -ForceCpu only for CPU diagnostics."
    }
}

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$work = Join-Path $WorkRoot ("superzip-storage-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $inputRoot = Join-Path $work "input"
    $outputRoot = Join-Path $work "out"
    New-Item -ItemType Directory -Force -Path $inputRoot | Out-Null
    $inputFile = Join-Path $inputRoot "payload.bin"
    $archive = Join-Path $work "smoke.suzip"
    Write-SmokePayload -Path $inputFile -Bytes ([int64]$SizeMiB * 1MB)

    & $cli compress --format suzip $modeFlag --workers ([Math]::Min([Environment]::ProcessorCount, 64)) --output $archive $inputRoot | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "storage smoke compression failed"
    }
    & $cli verify $modeFlag --workers ([Math]::Min([Environment]::ProcessorCount, 64)) $archive | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "storage smoke verification failed"
    }
    & $cli extract --format suzip $modeFlag --workers ([Math]::Min([Environment]::ProcessorCount, 64)) --output $outputRoot $archive | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "storage smoke extraction failed"
    }

    $restored = Join-Path $outputRoot "input\payload.bin"
    if (-not (Test-Path $restored)) {
        throw "storage smoke restored file is missing"
    }
    $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $inputFile).Hash
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $restored).Hash
    if ($expectedHash -ne $actualHash) {
        throw "storage smoke SHA-256 mismatch"
    }
    Write-Host "Storage smoke passed with $SizeMiB MiB bounded filesystem workload."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
