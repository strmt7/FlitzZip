param(
    [string]$Configuration = "Release",
    [int64]$SizeMiB = 10240,
    [int]$Iterations = 3,
    [ValidateSet("Memory", "Filesystem")] [string]$Mode = "Memory",
    [Alias("Profile")]
    [ValidateSet("Mixed", "Compressible", "Incompressible")] [string]$WorkloadProfile = "Mixed",
    [ValidateRange(1, 9)] [int]$CompressionLevel = 5,
    [ValidateSet(256, 512, 1024, 2048, 4096, 8192, 16384)] [int[]]$BlockSizeKiB = @(256, 512, 1024, 2048, 4096, 8192, 16384),
    [ValidateRange(50, 5000)] [int]$SampleIntervalMs = 100,
    [string]$WorkRoot = $env:TEMP,
    [switch]$NoResourceCounters,
    [switch]$ShowOperationStats,
    [switch]$SkipCpu,
    [switch]$SkipGpu,
    [switch]$AllowLargeDiskWrites
)

# Purpose: Compare forced-CPU and required-AMD-HIP performance on the same generated SUZIP workload.
# Inputs: `Configuration` selects the built CLI, `SizeMiB` controls generated data size, `Iterations` controls repeated timed runs, `Mode` selects RAM-only performance benchmarking or bounded filesystem smoke, `WorkloadProfile` selects workload shape, `CompressionLevel` selects SUZIP deflate effort, `BlockSizeKiB` selects one or more production block-size choices, `SampleIntervalMs` controls resource counter cadence, `WorkRoot` controls temporary storage for explicit filesystem smoke, `ShowOperationStats` prints raw CLI stats, skip switches disable one lane, and `AllowLargeDiskWrites` is retained only to reject obsolete unsafe invocations.
# Outputs: Prints per-operation CPU/GPU throughput plus aggregate speedup; throws on correctness, lane-selection, memory-budget, or unsafe-disk-write failures.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
$MaxFilesystemSmokeMiB = 64
$script:ShowOperationStatsEnabled = [bool]$ShowOperationStats

# Purpose: Emit benchmark text through the output pipeline instead of host-only writes.
# Inputs: `Message` is a status, table, or diagnostic line.
# Outputs: Writes the text to the success stream.
function Write-BenchmarkMessage {
    param([Parameter(ValueFromPipeline = $true)][AllowEmptyString()][string]$Message)
    process {
        Write-Output $Message
    }
}
if (-not (Test-Path $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}
if ($SkipCpu -and $SkipGpu) {
    throw "At least one benchmark lane must run."
}
if ($AllowLargeDiskWrites) {
    throw "-AllowLargeDiskWrites is obsolete. Development benchmarks must be RAM-only; use -Mode Memory for CPU/GPU benchmarking and tools/storage_smoke.ps1 for the small filesystem path."
}
if ($Mode -eq "Memory" -and $SizeMiB -lt 10240) {
    throw "Benchmark workload must be at least 10240 MiB (10 GiB) for meaningful CPU/GPU comparison."
}
if ($Mode -eq "Filesystem" -and $SizeMiB -gt $MaxFilesystemSmokeMiB) {
    throw "Filesystem mode is limited to $MaxFilesystemSmokeMiB MiB and exists only as a bounded I/O smoke. Use the default -Mode Memory for CPU/GPU benchmarking."
}

try {
    if (-not ([System.Management.Automation.PSTypeName]"SuperZipProcessIoCounters").Type) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class SuperZipProcessIoCounters
{
    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetProcessIoCounters(IntPtr processHandle, out IO_COUNTERS counters);
}
"@
    }
    $script:ProcessIoCountersAvailable = $true
} catch {
    $script:ProcessIoCountersAvailable = $false
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

# Purpose: Read cumulative kernel process I/O counters.
# Inputs: `Process` is a live or just-exited process with a valid handle.
# Outputs: Returns read/write transfer byte counts; unavailable counters return nulls.
function Get-ProcessIoTransfer {
    param([Parameter(Mandatory = $true)][Diagnostics.Process]$Process)
    if (-not $script:ProcessIoCountersAvailable) {
        return [pscustomobject]@{
            ReadBytes = $null
            WriteBytes = $null
        }
    }
    try {
        $counters = New-Object SuperZipProcessIoCounters+IO_COUNTERS
        if ([SuperZipProcessIoCounters]::GetProcessIoCounters($Process.Handle, [ref]$counters)) {
            return [pscustomobject]@{
                ReadBytes = [double]$counters.ReadTransferCount
                WriteBytes = [double]$counters.WriteTransferCount
            }
        }
    } catch {
        Write-Verbose "GetProcessIoCounters failed: $($_.Exception.Message)"
    }
    return [pscustomobject]@{
        ReadBytes = $null
        WriteBytes = $null
    }
}

# Purpose: Resolve the Windows LogicalDisk performance-counter instance for the benchmark work root.
# Inputs: `Path` is an existing or intended directory on the target benchmark volume.
# Outputs: Returns a counter instance such as `C:`; returns `$null` if no local drive root is available.
function Get-LogicalDiskCounterInstance {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $rootPath = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($rootPath)) {
        return $null
    }
    $trimmed = $rootPath.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return $null
    }
    return $trimmed
}

# Purpose: Read logical-disk activity and throughput samples for the benchmark volume.
# Inputs: None; the script-level disk counter path list is initialized after the work root exists.
# Outputs: Returns active time plus read/write bytes per second; unavailable counters return nulls.
function Get-DiskResourceSample {
    $diskActive = $null
    $diskRead = $null
    $diskWrite = $null
    if (-not $script:BenchmarkDiskCounterPaths -or $script:BenchmarkDiskCounterPaths.Count -eq 0) {
        return [pscustomobject]@{
            DiskActive = $diskActive
            DiskReadBytesPerSec = $diskRead
            DiskWriteBytesPerSec = $diskWrite
        }
    }
    try {
        $counterSamples = (Get-Counter $script:BenchmarkDiskCounterPaths).CounterSamples
        foreach ($sample in $counterSamples) {
            if ($sample.Path.EndsWith("\% disk time", [StringComparison]::OrdinalIgnoreCase)) {
                $diskActive = [double]$sample.CookedValue
            } elseif ($sample.Path.EndsWith("\disk read bytes/sec", [StringComparison]::OrdinalIgnoreCase)) {
                $diskRead = [double]$sample.CookedValue
            } elseif ($sample.Path.EndsWith("\disk write bytes/sec", [StringComparison]::OrdinalIgnoreCase)) {
                $diskWrite = [double]$sample.CookedValue
            }
        }
    } catch {
        $diskActive = $null
        $diskRead = $null
        $diskWrite = $null
    }
    return [pscustomobject]@{
        DiskActive = $diskActive
        DiskReadBytesPerSec = $diskRead
        DiskWriteBytesPerSec = $diskWrite
    }
}

# Purpose: Read one short-interval resource sample for a running CLI process.
# Inputs: `ProcessId` is the superzip_cli process id to match against GPU engine counters and `CpuPct` is the caller-computed process CPU percentage.
# Outputs: Returns CPU, per-process aggregate GPU engine percent, and benchmark-volume disk counters; unavailable counters return `$null`.
function Get-ResourceSample {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [AllowNull()][double]$CpuPct
    )
    $gpu = $null
    try {
        $pidPattern = "pid_$ProcessId`_"
        $gpuSamples = (Get-Counter '\GPU Engine(*)\Utilization Percentage').CounterSamples |
            Where-Object { $_.Path.IndexOf($pidPattern, [StringComparison]::OrdinalIgnoreCase) -ge 0 }
        if ($gpuSamples) {
            $gpu = [double](($gpuSamples | Measure-Object CookedValue -Sum).Sum)
        }
    } catch {
        $gpu = $null
    }
    $disk = Get-DiskResourceSample
    return [pscustomobject]@{
        Cpu = $CpuPct
        Gpu = $gpu
        DiskActive = $disk.DiskActive
        DiskReadBytesPerSec = $disk.DiskReadBytesPerSec
        DiskWriteBytesPerSec = $disk.DiskWriteBytesPerSec
    }
}

# Purpose: Convert bytes per second to MiB per second.
# Inputs: `Value` is a byte-per-second counter sample.
# Outputs: Returns MiB/s, preserving null when the counter was unavailable.
function ConvertTo-MiBPerSecond {
    param([AllowNull()][double]$Value)
    if ($null -eq $Value) {
        return $null
    }
    return $Value / 1MB
}

# Purpose: Summarize sampled utilization values for one CLI operation.
# Inputs: `Samples` is a list emitted by `Get-ResourceSample`.
# Outputs: Returns average and peak CPU/GPU/disk values; missing counters are reported as nulls.
function Measure-ResourceSample {
    param([Parameter(Mandatory = $true)]$Samples)
    $cpuSamples = @($Samples | Where-Object { $null -ne $_.Cpu } | ForEach-Object { $_.Cpu })
    $gpuSamples = @($Samples | Where-Object { $null -ne $_.Gpu } | ForEach-Object { $_.Gpu })
    $diskActiveSamples = @($Samples | Where-Object { $null -ne $_.DiskActive } | ForEach-Object { $_.DiskActive })
    $diskReadSamples = @($Samples | Where-Object { $null -ne $_.DiskReadBytesPerSec } | ForEach-Object { ConvertTo-MiBPerSecond -Value $_.DiskReadBytesPerSec })
    $diskWriteSamples = @($Samples | Where-Object { $null -ne $_.DiskWriteBytesPerSec } | ForEach-Object { ConvertTo-MiBPerSecond -Value $_.DiskWriteBytesPerSec })
    $cpuAverage = if ($cpuSamples.Count -gt 0) { ($cpuSamples | Measure-Object -Average).Average } else { $null }
    $cpuPeak = if ($cpuSamples.Count -gt 0) { ($cpuSamples | Measure-Object -Maximum).Maximum } else { $null }
    $gpuAverage = if ($gpuSamples.Count -gt 0) { ($gpuSamples | Measure-Object -Average).Average } else { $null }
    $gpuPeak = if ($gpuSamples.Count -gt 0) { ($gpuSamples | Measure-Object -Maximum).Maximum } else { $null }
    $diskActiveAverage = if ($diskActiveSamples.Count -gt 0) { ($diskActiveSamples | Measure-Object -Average).Average } else { $null }
    $diskActivePeak = if ($diskActiveSamples.Count -gt 0) { ($diskActiveSamples | Measure-Object -Maximum).Maximum } else { $null }
    $diskReadAverage = if ($diskReadSamples.Count -gt 0) { ($diskReadSamples | Measure-Object -Average).Average } else { $null }
    $diskReadPeak = if ($diskReadSamples.Count -gt 0) { ($diskReadSamples | Measure-Object -Maximum).Maximum } else { $null }
    $diskWriteAverage = if ($diskWriteSamples.Count -gt 0) { ($diskWriteSamples | Measure-Object -Average).Average } else { $null }
    $diskWritePeak = if ($diskWriteSamples.Count -gt 0) { ($diskWriteSamples | Measure-Object -Maximum).Maximum } else { $null }
    return @{
        cpu_avg_pct = $cpuAverage
        cpu_peak_pct = $cpuPeak
        gpu_avg_pct = $gpuAverage
        gpu_peak_pct = $gpuPeak
        disk_active_avg_pct = $diskActiveAverage
        disk_active_peak_pct = $diskActivePeak
        disk_read_avg_mib_s = $diskReadAverage
        disk_read_peak_mib_s = $diskReadPeak
        disk_write_avg_mib_s = $diskWriteAverage
        disk_write_peak_mib_s = $diskWritePeak
    }
}

# Purpose: Average numeric values while preserving null when no counter data exists.
# Inputs: `Values` is a sequence that may include nulls.
# Outputs: Returns an average double or null when no numeric values are present.
function Get-OptionalAverage {
    param($Values)
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        return $null
    }
    return ($items | Measure-Object -Average).Average
}

# Purpose: Find the maximum numeric value while preserving null when no counter data exists.
# Inputs: `Values` is a sequence that may include nulls.
# Outputs: Returns a maximum double or null when no numeric values are present.
function Get-OptionalMaximum {
    param($Values)
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        return $null
    }
    return ($items | Measure-Object -Maximum).Maximum
}

# Purpose: Sum numeric values while preserving null when no counter data exists.
# Inputs: `Values` is a sequence that may include nulls.
# Outputs: Returns a numeric sum or null when no numeric values are present.
function Get-OptionalSum {
    param($Values)
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        return $null
    }
    return ($items | Measure-Object -Sum).Sum
}

# Purpose: Read a numeric CLI statistic from a parsed stats dictionary.
# Inputs: `Stats` is a dictionary returned by `ConvertFrom-StatsLine` and `Key` is the statistic name.
# Outputs: Returns a double value or null when the key is absent.
function Get-StatsNumber {
    param(
        [Parameter(Mandatory = $true)]$Stats,
        [Parameter(Mandatory = $true)][string]$Key
    )
    if (-not $Stats.ContainsKey($Key)) {
        return $null
    }
    return [double]$Stats[$Key]
}

# Purpose: Assert that a required-GPU benchmark operation really submitted AMD HIP work.
# Inputs: `Stats` is a parsed CLI statistics dictionary and `Label` identifies the operation in diagnostics.
# Outputs: Returns normally when backend HIP telemetry proves execution; throws on possible CPU fallback or missing telemetry.
function Assert-GpuBackendStat {
    param(
        [Parameter(Mandatory = $true)]$Stats,
        [Parameter(Mandatory = $true)][string]$Label,
        [bool]$RequirePatternBlocks = $false
    )
    if ($Stats["gpu_used"] -ne "true") {
        throw "$Label did not report gpu_used=true in the required-GPU lane."
    }
    $kernelLaunches = Get-StatsNumber -Stats $Stats -Key "gpu_kernel_launches"
    $kernelMs = Get-StatsNumber -Stats $Stats -Key "gpu_kernel_ms"
    $h2dBytes = Get-StatsNumber -Stats $Stats -Key "gpu_h2d_bytes"
    $allocBytes = Get-StatsNumber -Stats $Stats -Key "gpu_device_allocation_bytes"
    if ($null -eq $kernelLaunches -or $kernelLaunches -le 0) {
        throw "$Label reported no AMD HIP kernel launches in the required-GPU lane."
    }
    if ($null -eq $kernelMs -or $kernelMs -le 0) {
        throw "$Label reported no AMD HIP event time in the required-GPU lane."
    }
    if ($null -eq $h2dBytes -or $h2dBytes -le 0) {
        throw "$Label reported no host-to-device transfer bytes in the required-GPU lane."
    }
    if ($null -eq $allocBytes -or $allocBytes -le 0) {
        throw "$Label reported no AMD HIP device allocation bytes in the required-GPU lane."
    }
    if ($RequirePatternBlocks -and (Get-StatsNumber -Stats $Stats -Key "gpu_pattern_blocks") -le 0) {
        throw "$Label reported no GPU-compressed pattern blocks in the required-GPU lane."
    }
}

# Purpose: Run a CLI command, sample resource utilization, and parse operation statistics.
# Inputs: `Arguments` is the exact CLI argument vector to execute.
# Outputs: Returns parsed statistics plus utilization samples; throws if the command fails.
function Invoke-SuperZipStat {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    if ($NoResourceCounters) {
        $output = & $cli @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "superzip_cli failed with exit code $LASTEXITCODE while running: $($Arguments -join ' ')"
        }
        $statsLine = $output | Where-Object { $_ -match '^entries=' } | Select-Object -Last 1
        if ($script:ShowOperationStatsEnabled) {
            Write-BenchmarkMessage "operation_stats $($Arguments -join ' ') :: $statsLine"
        }
        $stats = ConvertFrom-StatsLine -Line $statsLine
        foreach ($key in @(
            "cpu_avg_pct",
            "cpu_peak_pct",
            "gpu_avg_pct",
            "gpu_peak_pct",
            "disk_active_avg_pct",
            "disk_active_peak_pct",
            "disk_read_avg_mib_s",
            "disk_read_peak_mib_s",
            "disk_write_avg_mib_s",
            "disk_write_peak_mib_s",
            "process_read_mib",
            "process_write_mib"
        )) {
            $stats[$key] = $null
        }
        return $stats
    }

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $cli
    $psi.Arguments = (($Arguments | ForEach-Object { ConvertTo-WindowsArgument -Value $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($psi)
    $initialIo = Get-ProcessIoTransfer -Process $process
    $samples = [System.Collections.Generic.List[object]]::new()
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $logicalProcessors = [Math]::Max(1, [Environment]::ProcessorCount)
    $lastCpuMs = $process.TotalProcessorTime.TotalMilliseconds
    $lastWallMs = $clock.Elapsed.TotalMilliseconds
    while (-not $process.HasExited) {
        Start-Sleep -Milliseconds $SampleIntervalMs
        $process.Refresh()
        $nowCpuMs = $process.TotalProcessorTime.TotalMilliseconds
        $nowWallMs = $clock.Elapsed.TotalMilliseconds
        $cpuPct = $null
        $deltaWallMs = $nowWallMs - $lastWallMs
        if ($deltaWallMs -gt 0) {
            $cpuPct = (($nowCpuMs - $lastCpuMs) / $deltaWallMs / $logicalProcessors) * 100.0
        }
        $lastCpuMs = $nowCpuMs
        $lastWallMs = $nowWallMs
        $samples.Add((Get-ResourceSample -ProcessId $process.Id -CpuPct $cpuPct))
    }
    $finalIo = Get-ProcessIoTransfer -Process $process
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "superzip_cli failed with exit code $($process.ExitCode) while running: $($Arguments -join ' '): $stderr"
    }
    $statsLine = $stdout -split "`r?`n" | Where-Object { $_ -match '^entries=' } | Select-Object -Last 1
    if ($script:ShowOperationStatsEnabled) {
        Write-BenchmarkMessage "operation_stats $($Arguments -join ' ') :: $statsLine"
    }
    $stats = ConvertFrom-StatsLine -Line $statsLine
    $resourceStats = Measure-ResourceSample -Samples $samples
    foreach ($key in $resourceStats.Keys) {
        $stats[$key] = $resourceStats[$key]
    }
    if ($null -ne $initialIo.ReadBytes -and $null -ne $finalIo.ReadBytes) {
        $stats["process_read_mib"] = ($finalIo.ReadBytes - $initialIo.ReadBytes) / 1MB
    } else {
        $stats["process_read_mib"] = $null
    }
    if ($null -ne $initialIo.WriteBytes -and $null -ne $finalIo.WriteBytes) {
        $stats["process_write_mib"] = ($finalIo.WriteBytes - $initialIo.WriteBytes) / 1MB
    } else {
        $stats["process_write_mib"] = $null
    }
    return $stats
}

# Purpose: Query available bytes on the filesystem backing a path.
# Inputs: `Path` is an existing or intended directory.
# Outputs: Returns free bytes reported by the volume.
function Get-AvailableByte {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $rootPath = [IO.Path]::GetPathRoot($fullPath)
    $drive = New-Object IO.DriveInfo($rootPath)
    return [int64]$drive.AvailableFreeSpace
}

# Purpose: Fail before generating a large benchmark when the temp volume is too small.
# Inputs: `Path` is the temporary root, `SizeMiB` is the requested source data size, and `LaneCount` is the number of measured lanes.
# Outputs: Throws with a clear diagnostic if free disk space is insufficient.
function Assert-BenchmarkDiskBudget {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$SizeMiB,
        [Parameter(Mandatory = $true)][int]$LaneCount
    )
    if ($LaneCount -le 0) {
        throw "At least one benchmark lane must run."
    }
    $sourceBytes = [int64]$SizeMiB * 1MB
    $peakArchiveBytes = $sourceBytes
    $peakExtractBytes = $sourceBytes
    $safetyBytes = 2GB
    $requiredBytes = $sourceBytes + $peakArchiveBytes + $peakExtractBytes + $safetyBytes
    $availableBytes = Get-AvailableByte -Path $Path
    if ($availableBytes -lt $requiredBytes) {
        $requiredGiB = [Math]::Ceiling($requiredBytes / 1GB)
        $availableGiB = [Math]::Round($availableBytes / 1GB, 1)
        throw "Benchmark requires at least $requiredGiB GiB free under '$Path'; available free space is $availableGiB GiB."
    }
}

# Purpose: Write deterministic bytes to a file without holding the whole benchmark data set in RAM.
# Inputs: `Path` is the output file, `Bytes` is the target length, and `Kind` selects zero, random, or text-like data.
# Outputs: Creates or overwrites the file with deterministic content.
function Write-BenchmarkFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Bytes,
        [Parameter(Mandatory = $true)][ValidateSet("Zero", "Random", "Text")] [string]$Kind
    )
    $bufferSize = 1MB
    $buffer = New-Object byte[] $bufferSize
    if ($Kind -eq "Random") {
        $rng = [System.Random]::new(1234567)
    } elseif ($Kind -eq "Text") {
        $line = [Text.Encoding]::UTF8.GetBytes("SuperZip benchmark line: archive metadata, mixed file sizes, AMD HIP comparison.`n")
        for ($i = 0; $i -lt $buffer.Length; $i++) {
            $buffer[$i] = $line[$i % $line.Length]
        }
    }

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([int64]$buffer.Length, $remaining)
            if ($Kind -eq "Random") {
                $rng.NextBytes($buffer)
            } elseif ($Kind -eq "Zero") {
                [Array]::Clear($buffer, 0, $count)
            }
            $stream.Write($buffer, 0, $count)
            $remaining -= $count
        }
    } finally {
        $stream.Dispose()
    }
}

# Purpose: Generate a selected archive workload with large files and many small entries.
# Inputs: `Root` is the benchmark input directory, `SizeMiB` is the approximate total payload size, and `Profile` selects data compressibility.
# Outputs: Creates deterministic files under `Root`.
function Initialize-BenchmarkDataset {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][int64]$SizeMiB,
        [Parameter(Mandatory = $true)][ValidateSet("Mixed", "Compressible", "Incompressible")] [string]$DataProfile
    )
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "small") | Out-Null

    $totalBytes = [int64]$SizeMiB * 1MB
    if ($DataProfile -eq "Compressible") {
        $zeroBytes = [int64]($totalBytes * 0.10)
        $textBytes = [int64]($totalBytes * 0.80)
    } elseif ($DataProfile -eq "Incompressible") {
        $zeroBytes = 0
        $textBytes = 0
    } else {
        $zeroBytes = [int64]($totalBytes * 0.25)
        $textBytes = [int64]($totalBytes * 0.25)
    }
    $randomBytes = [int64]($totalBytes - $zeroBytes - $textBytes - (4MB))
    if ($randomBytes -lt 1MB) {
        $randomBytes = 1MB
    }

    if ($zeroBytes -gt 0) {
        Write-BenchmarkFile -Path (Join-Path $Root "zeros.bin") -Bytes $zeroBytes -Kind Zero
    }
    if ($textBytes -gt 0) {
        Write-BenchmarkFile -Path (Join-Path $Root "text.log") -Bytes $textBytes -Kind Text
    }
    Write-BenchmarkFile -Path (Join-Path $Root "random.bin") -Bytes $randomBytes -Kind Random

    for ($i = 0; $i -lt 256; ++$i) {
        $smallPath = Join-Path $Root ("small\entry-{0:D4}.txt" -f $i)
        Set-Content -LiteralPath $smallPath -Value ("entry={0}; SuperZip benchmark metadata fanout" -f $i) -NoNewline
    }
}

# Purpose: Compute a relative path on Windows PowerShell without relying on newer .NET APIs.
# Inputs: `Root` is the directory prefix and `Path` is a child file path.
# Outputs: Returns the path below `Root`; throws if `Path` is outside the root.
function Get-RelativePathCompat {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside expected root: $Path"
    }
    return $pathFull.Substring($rootFull.Length)
}

# Purpose: Compare two directory trees by relative path, length, and SHA-256 digest.
# Inputs: `Expected` and `Actual` are directory roots.
# Outputs: Throws on any content mismatch; returns normally when trees match.
function Test-TreeEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual
    )
    $expectedFiles = Get-ChildItem -LiteralPath $Expected -Recurse -File | Sort-Object FullName
    $actualFiles = Get-ChildItem -LiteralPath $Actual -Recurse -File | Sort-Object FullName
    if ($expectedFiles.Count -ne $actualFiles.Count) {
        throw "Extracted file count mismatch: expected $($expectedFiles.Count), got $($actualFiles.Count)"
    }
    for ($i = 0; $i -lt $expectedFiles.Count; ++$i) {
        $expectedRelative = Get-RelativePathCompat -Root $Expected -Path $expectedFiles[$i].FullName
        $actualRelative = Get-RelativePathCompat -Root $Actual -Path $actualFiles[$i].FullName
        if ($expectedRelative -ne $actualRelative -or $expectedFiles[$i].Length -ne $actualFiles[$i].Length) {
            throw "Extracted file metadata mismatch at $expectedRelative"
        }
        $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $expectedFiles[$i].FullName).Hash
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $actualFiles[$i].FullName).Hash
        if ($expectedHash -ne $actualHash) {
            throw "Extracted file hash mismatch at $expectedRelative"
        }
    }
}

# Purpose: Execute one benchmark lane and enforce expected GPU usage.
# Inputs: `Lane` is the display name, `ModeFlag` is `--force-cpu` or `--require-gpu`, `SourceRoot` is the source tree, `Work` is the temporary root, and `BlockSizeKiB` selects the production archive block size.
# Outputs: Returns measured compress/verify/extract statistics.
function Invoke-BenchmarkLane {
    param(
        [Parameter(Mandatory = $true)][string]$Lane,
        [Parameter(Mandatory = $true)][string]$ModeFlag,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$Work,
        [Parameter(Mandatory = $true)][int]$Iteration,
        [Parameter(Mandatory = $true)][int]$BlockSizeKiB
    )
    $archive = Join-Path $Work ("{0}-{1}-{2}kib.suzip" -f $Lane.ToLowerInvariant(), $Iteration, $BlockSizeKiB)
    $outRoot = Join-Path $Work ("{0}-out-{1}-{2}kib" -f $Lane.ToLowerInvariant(), $Iteration, $BlockSizeKiB)
    $pipelineArgs = @("--workers", "$script:BenchmarkWorkerCount")
    $compressionArgs = @("--compression-level", "$CompressionLevel", "--block-size-kib", "$BlockSizeKiB")
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $outRoot -Recurse -Force -ErrorAction SilentlyContinue

    try {
        $compress = Invoke-SuperZipStat -Arguments (@("compress", "--format", "suzip", $ModeFlag) + $pipelineArgs + $compressionArgs + @("--output", $archive, $SourceRoot))
        $verify = Invoke-SuperZipStat -Arguments (@("verify", $ModeFlag) + $pipelineArgs + @($archive))
        $extract = Invoke-SuperZipStat -Arguments (@("extract", "--format", "suzip", $ModeFlag) + $pipelineArgs + @("--output", $outRoot, $archive))

        $expectedGpu = if ($ModeFlag -eq "--require-gpu") { "true" } else { "false" }
        foreach ($item in @($compress, $verify, $extract)) {
            if ($item["gpu_used"] -ne $expectedGpu) {
                throw "$Lane lane reported gpu_used=$($item["gpu_used"]) for $ModeFlag"
            }
        }
        if ($ModeFlag -eq "--require-gpu") {
            Assert-GpuBackendStat -Stats $compress -Label "$Lane compress" -RequirePatternBlocks ($WorkloadProfile -ne "Incompressible")
            Assert-GpuBackendStat -Stats $verify -Label "$Lane verify"
            Assert-GpuBackendStat -Stats $extract -Label "$Lane extract"
            if ((Get-StatsNumber -Stats $verify -Key "gpu_d2h_bytes") -le 0) {
                throw "$Lane verify reported no device-to-host bytes in the required-GPU lane."
            }
            if ((Get-StatsNumber -Stats $extract -Key "gpu_d2h_bytes") -le 0) {
                throw "$Lane extract reported no device-to-host bytes in the required-GPU lane."
            }
        }

        Test-TreeEqual -Expected $SourceRoot -Actual (Join-Path $outRoot (Split-Path -Leaf $SourceRoot))
        $operationStats = @($compress, $verify, $extract)
        return [pscustomobject]@{
            Lane = $Lane
            Iteration = $Iteration
            BlockSizeKiB = $BlockSizeKiB
            CompressSeconds = [double]$compress["seconds"]
            VerifySeconds = [double]$verify["seconds"]
            ExtractSeconds = [double]$extract["seconds"]
            Workers = [int]$compress["workers"]
            InflightChunks = [int]$compress["inflight_chunks"]
            CompressMiBs = [double]$compress["throughput_mib_s"]
            VerifyMiBs = [double]$verify["throughput_mib_s"]
            ExtractMiBs = [double]$extract["throughput_mib_s"]
            CompressionRatio = [double]$compress["compression_ratio"]
            CpuAvgPct = Get-OptionalAverage -Values ($operationStats | ForEach-Object { $_["cpu_avg_pct"] })
            CpuPeakPct = Get-OptionalMaximum -Values ($operationStats | ForEach-Object { $_["cpu_peak_pct"] })
            GpuAvgPct = Get-OptionalAverage -Values ($operationStats | ForEach-Object { $_["gpu_avg_pct"] })
            GpuPeakPct = Get-OptionalMaximum -Values ($operationStats | ForEach-Object { $_["gpu_peak_pct"] })
            DiskActiveAvgPct = Get-OptionalAverage -Values ($operationStats | ForEach-Object { $_["disk_active_avg_pct"] })
            DiskActivePeakPct = Get-OptionalMaximum -Values ($operationStats | ForEach-Object { $_["disk_active_peak_pct"] })
            DiskReadAvgMiBs = Get-OptionalAverage -Values ($operationStats | ForEach-Object { $_["disk_read_avg_mib_s"] })
            DiskReadPeakMiBs = Get-OptionalMaximum -Values ($operationStats | ForEach-Object { $_["disk_read_peak_mib_s"] })
            DiskWriteAvgMiBs = Get-OptionalAverage -Values ($operationStats | ForEach-Object { $_["disk_write_avg_mib_s"] })
            DiskWritePeakMiBs = Get-OptionalMaximum -Values ($operationStats | ForEach-Object { $_["disk_write_peak_mib_s"] })
            ProcessReadMiB = Get-OptionalSum -Values ($operationStats | ForEach-Object { $_["process_read_mib"] })
            ProcessWriteMiB = Get-OptionalSum -Values ($operationStats | ForEach-Object { $_["process_write_mib"] })
            GpuEncodeChunks = Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_encode_chunks" })
            GpuDecodeChunks = Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_decode_chunks" })
            GpuKernelLaunches = Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_kernel_launches" })
            GpuKernelMs = Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_kernel_ms" })
            GpuPatternBlocks = Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_pattern_blocks" })
            GpuH2DMiB = (Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_h2d_bytes" })) / 1MB
            GpuD2HMiB = (Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_d2h_bytes" })) / 1MB
            GpuAllocMiB = (Get-OptionalSum -Values ($operationStats | ForEach-Object { Get-StatsNumber -Stats $_ -Key "gpu_device_allocation_bytes" })) / 1MB
        }
    } finally {
        Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $outRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Purpose: Execute one memory-only benchmark lane and enforce expected GPU usage.
# Inputs: `Lane` is the display name, `ModeFlag` is `--force-cpu` or `--require-gpu`, and `BlockSizeKiB` selects the production archive block size.
# Outputs: Returns measured compress/verify/extract statistics without creating benchmark files.
function Invoke-MemoryBenchmarkLane {
    param(
        [Parameter(Mandatory = $true)][string]$Lane,
        [Parameter(Mandatory = $true)][string]$ModeFlag,
        [Parameter(Mandatory = $true)][int]$Iteration,
        [Parameter(Mandatory = $true)][int]$BlockSizeKiB
    )
    $stats = Invoke-SuperZipStat -Arguments @(
        "memory-benchmark",
        "--size-mib", "$SizeMiB",
        "--profile", $WorkloadProfile,
        $ModeFlag,
        "--workers", "$script:BenchmarkWorkerCount",
        "--block-size-kib", "$BlockSizeKiB",
        "--compression-level", "$CompressionLevel"
    )

    $expectedGpu = if ($ModeFlag -eq "--require-gpu") { "true" } else { "false" }
    if ($stats["gpu_used"] -ne $expectedGpu) {
        throw "$Lane memory benchmark reported gpu_used=$($stats["gpu_used"]) for $ModeFlag"
    }
    if ($stats["memory_only"] -ne "true") {
        throw "$Lane memory benchmark did not report memory_only=true."
    }
    if ([double]$stats["disk_write_bytes"] -ne 0) {
        throw "$Lane memory benchmark reported disk_write_bytes=$($stats["disk_write_bytes"])."
    }
    if ($ModeFlag -eq "--require-gpu") {
        Assert-GpuBackendStat -Stats $stats -Label "$Lane memory benchmark" -RequirePatternBlocks ($WorkloadProfile -ne "Incompressible")
    }

    return [pscustomobject]@{
        Lane = $Lane
        Iteration = $Iteration
        BlockSizeKiB = [int]($stats["block_size_bytes"] / 1KB)
        CompressSeconds = [double]$stats["compress_seconds"]
        VerifySeconds = [double]$stats["verify_seconds"]
        ExtractSeconds = [double]$stats["extract_seconds"]
        Workers = [int]$stats["workers"]
        InflightChunks = [int]$stats["inflight_chunks"]
        CodecWorkers = [int]$stats["codec_workers"]
        MemoryOnly = $stats["memory_only"]
        DiskWriteBytes = [double]$stats["disk_write_bytes"]
        CompressMiBs = [double]$stats["compress_mib_s"]
        VerifyMiBs = [double]$stats["verify_mib_s"]
        ExtractMiBs = [double]$stats["extract_mib_s"]
        CompressionRatio = [double]$stats["compression_ratio"]
        CpuAvgPct = $stats["cpu_avg_pct"]
        CpuPeakPct = $stats["cpu_peak_pct"]
        GpuAvgPct = $stats["gpu_avg_pct"]
        GpuPeakPct = $stats["gpu_peak_pct"]
        DiskActiveAvgPct = $null
        DiskActivePeakPct = $null
        DiskReadAvgMiBs = $null
        DiskReadPeakMiBs = $null
        DiskWriteAvgMiBs = $null
        DiskWritePeakMiBs = $null
        ProcessReadMiB = $stats["process_read_mib"]
        ProcessWriteMiB = $stats["process_write_mib"]
        GpuEncodeChunks = Get-StatsNumber -Stats $stats -Key "gpu_encode_chunks"
        GpuDecodeChunks = Get-StatsNumber -Stats $stats -Key "gpu_decode_chunks"
        GpuKernelLaunches = Get-StatsNumber -Stats $stats -Key "gpu_kernel_launches"
        GpuKernelMs = Get-StatsNumber -Stats $stats -Key "gpu_kernel_ms"
        GpuPatternBlocks = Get-StatsNumber -Stats $stats -Key "gpu_pattern_blocks"
        GpuH2DMiB = (Get-StatsNumber -Stats $stats -Key "gpu_h2d_bytes") / 1MB
        GpuD2HMiB = (Get-StatsNumber -Stats $stats -Key "gpu_d2h_bytes") / 1MB
        GpuAllocMiB = (Get-StatsNumber -Stats $stats -Key "gpu_device_allocation_bytes") / 1MB
    }
}

$laneCount = 0
if (-not $SkipCpu) { ++$laneCount }
if (-not $SkipGpu) { ++$laneCount }
$script:BenchmarkWorkerCount = [Math]::Min([Environment]::ProcessorCount, 64)
if ($Mode -eq "Memory") {
    $script:BenchmarkDiskCounterPaths = @()
    if (-not $SkipGpu) {
        & $cli dependency-check | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "GPU benchmark requires a HIP build with an available AMD GPU."
        }
    }

    $results = @()
    for ($iteration = 1; $iteration -le [Math]::Max(1, $Iterations); ++$iteration) {
        foreach ($blockSize in $BlockSizeKiB) {
            if (-not $SkipCpu) {
                $results += Invoke-MemoryBenchmarkLane -Lane "CPU" -ModeFlag "--force-cpu" -Iteration $iteration -BlockSizeKiB $blockSize
            }
            if (-not $SkipGpu) {
                $results += Invoke-MemoryBenchmarkLane -Lane "GPU" -ModeFlag "--require-gpu" -Iteration $iteration -BlockSizeKiB $blockSize
            }
        }
    }

    $summary = $results | Group-Object Lane, BlockSizeKiB | ForEach-Object {
        $group = $_.Group
        $totalSeconds = ($group | ForEach-Object { $_.CompressSeconds + $_.VerifySeconds + $_.ExtractSeconds } | Measure-Object -Average).Average
        $gpuKernelMs = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuKernelMs })
        $gpuKernelDutyPct = if ($null -ne $gpuKernelMs -and $totalSeconds -gt 0) {
            ($gpuKernelMs / ($totalSeconds * 1000.0)) * 100.0
        } else {
            $null
        }
        [pscustomobject]@{
            Lane = $group[0].Lane
            BlockSizeKiB = $group[0].BlockSizeKiB
            Iterations = $_.Count
            Workers = ($group | Measure-Object Workers -Maximum).Maximum
            InflightChunks = ($group | Measure-Object InflightChunks -Maximum).Maximum
            CodecWorkers = ($group | Measure-Object CodecWorkers -Maximum).Maximum
            MemoryOnly = (($group | ForEach-Object { $_.MemoryOnly } | Sort-Object -Unique) -join ",")
            DiskWriteBytes = Get-OptionalAverage -Values ($group | ForEach-Object { $_.DiskWriteBytes })
            CompressMiBs = ($group | Measure-Object CompressMiBs -Average).Average
            VerifyMiBs = ($group | Measure-Object VerifyMiBs -Average).Average
            ExtractMiBs = ($group | Measure-Object ExtractMiBs -Average).Average
            CompressionRatio = ($group | Measure-Object CompressionRatio -Average).Average
            TotalSeconds = $totalSeconds
            CpuAvgPct = Get-OptionalAverage -Values ($group | ForEach-Object { $_.CpuAvgPct })
            CpuPeakPct = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.CpuPeakPct })
            GpuAvgPct = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuAvgPct })
            GpuPeakPct = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.GpuPeakPct })
            DiskActiveAvgPct = $null
            DiskActivePeakPct = $null
            DiskReadAvgMiBs = $null
            DiskReadPeakMiBs = $null
            DiskWriteAvgMiBs = $null
            DiskWritePeakMiBs = $null
            ProcessReadMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.ProcessReadMiB })
            ProcessWriteMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.ProcessWriteMiB })
            GpuEncodeChunks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuEncodeChunks })
            GpuDecodeChunks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuDecodeChunks })
            GpuKernelLaunches = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuKernelLaunches })
            GpuKernelMs = $gpuKernelMs
            GpuKernelDutyPct = $gpuKernelDutyPct
            GpuPatternBlocks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuPatternBlocks })
            GpuH2DMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuH2DMiB })
            GpuD2HMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuD2HMiB })
            GpuAllocMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuAllocMiB })
        }
    }

    Write-BenchmarkMessage ""
    Write-BenchmarkMessage "SuperZip benchmark: $SizeMiB MiB $WorkloadProfile SUZIP memory-only workload, compression level $CompressionLevel, block sizes $($BlockSizeKiB -join ',') KiB, $([Math]::Max(1, $Iterations)) iteration(s)"
    if ($NoResourceCounters) {
        Write-BenchmarkMessage "Resource sampling: disabled"
    } else {
        Write-BenchmarkMessage "Resource sampling: ${SampleIntervalMs} ms interval; logical-disk counters disabled in memory-only mode"
    }
    Write-BenchmarkMessage "Performance:"
    $summary |
        Sort-Object Lane, BlockSizeKiB |
        Select-Object Lane, BlockSizeKiB, Iterations, TotalSeconds, Workers, InflightChunks, CodecWorkers, MemoryOnly, DiskWriteBytes, CompressionRatio, CompressMiBs, VerifyMiBs, ExtractMiBs |
        Format-Table -AutoSize |
        Out-String -Width 320 |
        Write-BenchmarkMessage

    Write-BenchmarkMessage "Resource telemetry:"
    $summary |
        Sort-Object Lane, BlockSizeKiB |
        Select-Object Lane, BlockSizeKiB, CpuAvgPct, CpuPeakPct, GpuAvgPct, GpuPeakPct, DiskActiveAvgPct, DiskActivePeakPct, DiskReadAvgMiBs, DiskReadPeakMiBs, DiskWriteAvgMiBs, DiskWritePeakMiBs, ProcessReadMiB, ProcessWriteMiB, GpuEncodeChunks, GpuDecodeChunks, GpuKernelLaunches, GpuKernelMs, GpuKernelDutyPct, GpuPatternBlocks, GpuH2DMiB, GpuD2HMiB, GpuAllocMiB |
        Format-List |
        Out-String -Width 320 |
        Write-BenchmarkMessage

    foreach ($blockSize in $BlockSizeKiB) {
        $cpu = $summary | Where-Object { $_.Lane -eq "CPU" -and $_.BlockSizeKiB -eq $blockSize } | Select-Object -First 1
        $gpu = $summary | Where-Object { $_.Lane -eq "GPU" -and $_.BlockSizeKiB -eq $blockSize } | Select-Object -First 1
        if (-not ($cpu -and $gpu)) {
            continue
        }
        $speedup = $cpu.TotalSeconds / $gpu.TotalSeconds
        Write-BenchmarkMessage ("GPU end-to-end speedup vs forced CPU at {0} KiB blocks: {1:N2}x" -f $blockSize, $speedup)
        if ($speedup -lt 1.0) {
            Write-BenchmarkMessage "Note: required-HIP was slower than forced CPU on this memory workload; treat that as a real optimization finding, not a pass/fail benchmark target."
        }
    }
    return
}
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$script:BenchmarkDiskInstance = Get-LogicalDiskCounterInstance -Path $WorkRoot
$script:BenchmarkDiskCounterPaths = @()
if ($script:BenchmarkDiskInstance) {
    $script:BenchmarkDiskCounterPaths = @(
        "\LogicalDisk($script:BenchmarkDiskInstance)\% Disk Time",
        "\LogicalDisk($script:BenchmarkDiskInstance)\Disk Read Bytes/sec",
        "\LogicalDisk($script:BenchmarkDiskInstance)\Disk Write Bytes/sec"
    )
}
Assert-BenchmarkDiskBudget -Path $WorkRoot -SizeMiB $SizeMiB -LaneCount $laneCount

$work = Join-Path $WorkRoot ("superzip-bench-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $sourceRoot = Join-Path $work "input"
    Initialize-BenchmarkDataset -Root $sourceRoot -SizeMiB $SizeMiB -DataProfile $WorkloadProfile

    if (-not $SkipGpu) {
        & $cli dependency-check | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "GPU benchmark requires a HIP build with an available AMD GPU."
        }
    }

    $results = @()
    for ($iteration = 1; $iteration -le [Math]::Max(1, $Iterations); ++$iteration) {
        foreach ($blockSize in $BlockSizeKiB) {
            if (-not $SkipCpu) {
                $results += Invoke-BenchmarkLane -Lane "CPU" -ModeFlag "--force-cpu" -SourceRoot $sourceRoot -Work $work -Iteration $iteration -BlockSizeKiB $blockSize
            }
            if (-not $SkipGpu) {
                $results += Invoke-BenchmarkLane -Lane "GPU" -ModeFlag "--require-gpu" -SourceRoot $sourceRoot -Work $work -Iteration $iteration -BlockSizeKiB $blockSize
            }
        }
    }

    $summary = $results | Group-Object Lane, BlockSizeKiB | ForEach-Object {
        $group = $_.Group
        $totalSeconds = ($group | ForEach-Object { $_.CompressSeconds + $_.VerifySeconds + $_.ExtractSeconds } | Measure-Object -Average).Average
        $gpuKernelMs = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuKernelMs })
        $gpuKernelDutyPct = if ($null -ne $gpuKernelMs -and $totalSeconds -gt 0) {
            ($gpuKernelMs / ($totalSeconds * 1000.0)) * 100.0
        } else {
            $null
        }
        [pscustomobject]@{
            Lane = $group[0].Lane
            BlockSizeKiB = $group[0].BlockSizeKiB
            Iterations = $_.Count
            Workers = ($group | Measure-Object Workers -Maximum).Maximum
            InflightChunks = ($group | Measure-Object InflightChunks -Maximum).Maximum
            CompressMiBs = ($group | Measure-Object CompressMiBs -Average).Average
            VerifyMiBs = ($group | Measure-Object VerifyMiBs -Average).Average
            ExtractMiBs = ($group | Measure-Object ExtractMiBs -Average).Average
            CompressionRatio = ($group | Measure-Object CompressionRatio -Average).Average
            TotalSeconds = $totalSeconds
            CpuAvgPct = Get-OptionalAverage -Values ($group | ForEach-Object { $_.CpuAvgPct })
            CpuPeakPct = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.CpuPeakPct })
            GpuAvgPct = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuAvgPct })
            GpuPeakPct = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.GpuPeakPct })
            DiskActiveAvgPct = Get-OptionalAverage -Values ($group | ForEach-Object { $_.DiskActiveAvgPct })
            DiskActivePeakPct = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.DiskActivePeakPct })
            DiskReadAvgMiBs = Get-OptionalAverage -Values ($group | ForEach-Object { $_.DiskReadAvgMiBs })
            DiskReadPeakMiBs = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.DiskReadPeakMiBs })
            DiskWriteAvgMiBs = Get-OptionalAverage -Values ($group | ForEach-Object { $_.DiskWriteAvgMiBs })
            DiskWritePeakMiBs = Get-OptionalMaximum -Values ($group | ForEach-Object { $_.DiskWritePeakMiBs })
            ProcessReadMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.ProcessReadMiB })
            ProcessWriteMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.ProcessWriteMiB })
            GpuEncodeChunks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuEncodeChunks })
            GpuDecodeChunks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuDecodeChunks })
            GpuKernelLaunches = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuKernelLaunches })
            GpuKernelMs = $gpuKernelMs
            GpuKernelDutyPct = $gpuKernelDutyPct
            GpuPatternBlocks = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuPatternBlocks })
            GpuH2DMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuH2DMiB })
            GpuD2HMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuD2HMiB })
            GpuAllocMiB = Get-OptionalAverage -Values ($group | ForEach-Object { $_.GpuAllocMiB })
        }
    }

    Write-BenchmarkMessage ""
    Write-BenchmarkMessage "SuperZip benchmark: $SizeMiB MiB $WorkloadProfile SUZIP workload, compression level $CompressionLevel, block sizes $($BlockSizeKiB -join ',') KiB, $([Math]::Max(1, $Iterations)) iteration(s)"
    if ($NoResourceCounters) {
        Write-BenchmarkMessage "Resource sampling: disabled"
    } else {
        $diskLabel = if ($script:BenchmarkDiskInstance) { $script:BenchmarkDiskInstance } else { "unavailable" }
        Write-BenchmarkMessage "Resource sampling: ${SampleIntervalMs} ms interval; disk counter instance: $diskLabel"
    }
    Write-BenchmarkMessage "Performance:"
    $summary |
        Sort-Object Lane, BlockSizeKiB |
        Select-Object Lane, BlockSizeKiB, Iterations, Workers, InflightChunks, CompressionRatio, CompressMiBs, VerifyMiBs, ExtractMiBs, TotalSeconds |
        Format-Table -AutoSize |
        Out-String -Width 220 |
        Write-BenchmarkMessage

    Write-BenchmarkMessage "Resource telemetry:"
    $summary |
        Sort-Object Lane, BlockSizeKiB |
        Select-Object Lane, BlockSizeKiB, CpuAvgPct, CpuPeakPct, GpuAvgPct, GpuPeakPct, DiskActiveAvgPct, DiskActivePeakPct, DiskReadAvgMiBs, DiskReadPeakMiBs, DiskWriteAvgMiBs, DiskWritePeakMiBs, ProcessReadMiB, ProcessWriteMiB, GpuEncodeChunks, GpuDecodeChunks, GpuKernelLaunches, GpuKernelMs, GpuKernelDutyPct, GpuPatternBlocks, GpuH2DMiB, GpuD2HMiB, GpuAllocMiB |
        Format-List |
        Out-String -Width 220 |
        Write-BenchmarkMessage

    foreach ($blockSize in $BlockSizeKiB) {
        $cpu = $summary | Where-Object { $_.Lane -eq "CPU" -and $_.BlockSizeKiB -eq $blockSize } | Select-Object -First 1
        $gpu = $summary | Where-Object { $_.Lane -eq "GPU" -and $_.BlockSizeKiB -eq $blockSize } | Select-Object -First 1
        if (-not ($cpu -and $gpu)) {
            continue
        }
        $speedup = $cpu.TotalSeconds / $gpu.TotalSeconds
        Write-BenchmarkMessage ("GPU end-to-end speedup vs forced CPU at {0} KiB blocks: {1:N2}x" -f $blockSize, $speedup)
        if ($speedup -lt 1.0) {
            Write-BenchmarkMessage "Note: required-HIP was slower than forced CPU on this workload; treat that as a real optimization finding, not a pass/fail benchmark target."
        }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
