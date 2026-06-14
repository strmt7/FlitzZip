param(
    [string]$Configuration = "Release",
    [ValidateRange(1, 30)] [int]$Seconds = 8,
    [ValidateRange(16, 512)] [int]$BufferMiB = 256,
    [ValidateRange(1, 4096)] [int]$InnerIterations = 512,
    [ValidateRange(50, 1000)] [int]$SampleIntervalMs = 50
)

# Purpose: Run a HIP-only SuperZip diagnostic while independently sampling Windows GPU counters.
# Inputs: `Configuration` selects the CLI binary, diagnostic parameters control device work, and `SampleIntervalMs` controls external sampling cadence.
# Outputs: Prints HIP event telemetry plus independent CPU/GPU/memory samples; throws when HIP execution is not proven.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}

# Purpose: Parse a key/value block emitted by the diagnostic CLI.
# Inputs: `Lines` is stdout from `superzip_cli gpu-diagnostic`.
# Outputs: Returns a dictionary of parsed keys and values.
function ConvertFrom-KeyValueLines {
    param([Parameter(Mandatory = $true)][string[]]$Lines)
    $result = @{}
    foreach ($line in $Lines) {
        $pair = $line -split "=", 2
        if ($pair.Count -eq 2) {
            $result[$pair[0]] = $pair[1]
        }
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

# Purpose: Read Windows GPU utilization and memory samples for one process.
# Inputs: `ProcessId` is the diagnostic process id.
# Outputs: Returns GPU engine utilization and process GPU memory when counters are available.
function Get-GpuDiagnosticSample {
    param([Parameter(Mandatory = $true)][int]$ProcessId)
    $gpu = $null
    $gpuMemoryBytes = $null
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
    try {
        $pidPattern = "pid_$ProcessId"
        $memorySamples = (Get-Counter '\GPU Process Memory(*)\Local Usage').CounterSamples |
            Where-Object { $_.Path.IndexOf($pidPattern, [StringComparison]::OrdinalIgnoreCase) -ge 0 }
        if ($memorySamples) {
            $gpuMemoryBytes = [double](($memorySamples | Measure-Object CookedValue -Sum).Sum)
        }
    } catch {
        $gpuMemoryBytes = $null
    }
    return [pscustomobject]@{
        Gpu = $gpu
        GpuMemoryBytes = $gpuMemoryBytes
    }
}

# Purpose: Return the average of non-null numeric values.
# Inputs: `Values` may include nulls.
# Outputs: Returns null when no numeric values exist.
function Get-OptionalAverage {
    param($Values)
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        return $null
    }
    return ($items | Measure-Object -Average).Average
}

# Purpose: Return the maximum of non-null numeric values.
# Inputs: `Values` may include nulls.
# Outputs: Returns null when no numeric values exist.
function Get-OptionalMaximum {
    param($Values)
    $items = @($Values | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        return $null
    }
    return ($items | Measure-Object -Maximum).Maximum
}

$arguments = @(
    "gpu-diagnostic",
    "--seconds", "$Seconds",
    "--buffer-mib", "$BufferMiB",
    "--inner-iterations", "$InnerIterations"
)

$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $cli
$psi.Arguments = (($arguments | ForEach-Object { ConvertTo-WindowsArgument -Value $_ }) -join ' ')
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($psi)
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
    $gpu = Get-GpuDiagnosticSample -ProcessId $process.Id
    $samples.Add([pscustomobject]@{
        Cpu = $cpuPct
        Gpu = $gpu.Gpu
        GpuMemoryBytes = $gpu.GpuMemoryBytes
    })
}

$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
    throw "gpu-diagnostic failed with exit code $($process.ExitCode): $stderr"
}

$lines = $stdout -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }
$stats = ConvertFrom-KeyValueLines -Lines $lines
foreach ($line in $lines) {
    Write-Host $line
}

if ([double]$stats["diagnostic_kernel_launches"] -le 0 -or
    [double]$stats["diagnostic_kernel_ms"] -le 0 -or
    [double]$stats["diagnostic_h2d_bytes"] -le 0 -or
    [double]$stats["diagnostic_d2h_bytes"] -le 0 -or
    [double]$stats["diagnostic_device_allocation_bytes"] -le 0 -or
    [double]$stats["diagnostic_checksum"] -le 0) {
    throw "HIP diagnostic did not prove device execution."
}

$gpuMemoryAvgMiB = (Get-OptionalAverage -Values ($samples | ForEach-Object { $_.GpuMemoryBytes })) / 1MB
$gpuMemoryPeakMiB = (Get-OptionalMaximum -Values ($samples | ForEach-Object { $_.GpuMemoryBytes })) / 1MB
[pscustomobject]@{
    CpuAvgPct = Get-OptionalAverage -Values ($samples | ForEach-Object { $_.Cpu })
    CpuPeakPct = Get-OptionalMaximum -Values ($samples | ForEach-Object { $_.Cpu })
    WindowsGpuAvgPct = Get-OptionalAverage -Values ($samples | ForEach-Object { $_.Gpu })
    WindowsGpuPeakPct = Get-OptionalMaximum -Values ($samples | ForEach-Object { $_.Gpu })
    WindowsGpuMemoryAvgMiB = $gpuMemoryAvgMiB
    WindowsGpuMemoryPeakMiB = $gpuMemoryPeakMiB
    SampleCount = $samples.Count
} | Format-List | Out-String -Width 220 | Write-Host

Write-Host "GPU diagnostic passed: HIP event timing, transfers, allocation, and device checksum are nonzero."
