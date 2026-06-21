param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [ValidateSet("Mixed", "Compressible", "Incompressible")]
    [string]$WorkloadProfile = "Mixed",
    [int]$SizeMiB = 10240,
    [ValidateRange(1, 9)]
    [int]$CompressionLevel = 5,
    [ValidateSet(256, 512, 1024, 2048, 4096, 8192, 16384)]
    [int]$BlockSizeKiB = 1024
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "superzip_cli.exe not found. Run tools/build.ps1 -Configuration $Configuration first."
}

# Purpose: Convert one stable SuperZip key/value statistics line into a dictionary.
# Inputs: `Line` is the stdout line that begins with `entries=`.
# Outputs: Returns a string dictionary keyed by statistic name.
function ConvertFrom-SuperZipStatsLine {
    param([Parameter(Mandatory = $true)][string]$Line)

    $stats = @{}
    foreach ($token in ($Line -split "\s+")) {
        if (-not $token.Contains("=")) {
            continue
        }
        $pair = $token.Split("=", 2)
        $stats[$pair[0]] = $pair[1]
    }
    return $stats
}

# Purpose: Read a numeric SuperZip statistic as a double.
# Inputs: `Stats` is a key/value dictionary and `Name` identifies the required key.
# Outputs: Returns the numeric value or throws when the key is missing or invalid.
function Get-SuperZipStatNumber {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Stats,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not $Stats.ContainsKey($Name)) {
        throw "Missing SuperZip statistic: $Name"
    }
    return [double]::Parse([string]$Stats[$Name], [Globalization.CultureInfo]::InvariantCulture)
}

# Purpose: Run one RAM-only memory benchmark lane and collect transfer counters.
# Inputs: `LaneName` labels output and `ModeFlag` is `--force-cpu` or `--require-gpu`.
# Outputs: Returns parsed statistics after enforcing RAM-only and zero disk-write guarantees.
function Invoke-TransferDiagnosticLane {
    param(
        [Parameter(Mandatory = $true)][string]$LaneName,
        [Parameter(Mandatory = $true)][string]$ModeFlag
    )

    $arguments = @(
        "memory-benchmark",
        "--size-mib", [string]$SizeMiB,
        "--profile", $WorkloadProfile,
        "--compression-level", [string]$CompressionLevel,
        "--block-size-kib", [string]$BlockSizeKiB,
        $ModeFlag
    )
    $output = & $exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$LaneName transfer diagnostic failed with exit code $LASTEXITCODE.`n$output"
    }
    $statsLine = $output | Where-Object { $_ -match '^entries=' } | Select-Object -Last 1
    if (-not $statsLine) {
        throw "$LaneName transfer diagnostic did not print a SuperZip stats line."
    }
    $stats = ConvertFrom-SuperZipStatsLine -Line $statsLine
    if ($stats["memory_only"] -ne "true") {
        throw "$LaneName transfer diagnostic must stay RAM-only."
    }
    if ((Get-SuperZipStatNumber -Stats $stats -Name "disk_write_bytes") -ne 0.0) {
        throw "$LaneName transfer diagnostic wrote benchmark payload bytes to disk."
    }
    return $stats
}

$cpu = Invoke-TransferDiagnosticLane -LaneName "CPU" -ModeFlag "--force-cpu"
$gpu = Invoke-TransferDiagnosticLane -LaneName "GPU" -ModeFlag "--require-gpu"
if ($gpu["gpu_used"] -ne "true") {
    throw "GPU transfer diagnostic did not execute the required AMD HIP lane."
}
if ((Get-SuperZipStatNumber -Stats $gpu -Name "gpu_h2d_bytes") -le 0.0 -or
    (Get-SuperZipStatNumber -Stats $gpu -Name "gpu_d2h_bytes") -le 0.0 -or
    (Get-SuperZipStatNumber -Stats $gpu -Name "gpu_device_allocation_bytes") -le 0.0) {
    throw "GPU transfer diagnostic did not report nonzero H2D, D2H, and device allocation counters."
}

$summary = [ordered]@{
    profile = $WorkloadProfile
    size_mib = $SizeMiB
    compression_level = $CompressionLevel
    block_size_kib = $BlockSizeKiB
    cpu_input_bytes = [uint64](Get-SuperZipStatNumber -Stats $cpu -Name "input_bytes")
    cpu_output_bytes = [uint64](Get-SuperZipStatNumber -Stats $cpu -Name "output_bytes")
    cpu_compression_ratio = Get-SuperZipStatNumber -Stats $cpu -Name "compression_ratio"
    cpu_throughput_mib_s = Get-SuperZipStatNumber -Stats $cpu -Name "throughput_mib_s"
    gpu_input_bytes = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "input_bytes")
    gpu_output_bytes = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "output_bytes")
    gpu_compression_ratio = Get-SuperZipStatNumber -Stats $gpu -Name "compression_ratio"
    gpu_throughput_mib_s = Get-SuperZipStatNumber -Stats $gpu -Name "throughput_mib_s"
    gpu_h2d_bytes = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "gpu_h2d_bytes")
    gpu_d2h_bytes = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "gpu_d2h_bytes")
    gpu_device_allocation_bytes = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "gpu_device_allocation_bytes")
    gpu_kernel_launches = [uint64](Get-SuperZipStatNumber -Stats $gpu -Name "gpu_kernel_launches")
    gpu_kernel_ms = Get-SuperZipStatNumber -Stats $gpu -Name "gpu_kernel_ms"
    memory_only = $true
    disk_write_bytes = 0
}

$summary | ConvertTo-Json -Depth 3
