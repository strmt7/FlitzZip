param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$cli = Join-Path (Split-Path -Parent $PSScriptRoot) "build/$Configuration/superzip_cli.exe"
. (Join-Path $PSScriptRoot 'benchmark_statistics.ps1')

# Purpose: Probe native planning and invalid admission without executing a multi-gigabyte workload.
# Inputs: Arguments and the planning/capability command are controlled fixtures; subprocess lifetime/output are bounded.
# Outputs: Returns exit code and complete stdout/stderr, or terminates only the owned process on failure.
function Invoke-MemoryBenchmarkPlanProbe {
    param([string]$Arguments, [ValidateSet('memory-benchmark --plan-only', 'gpu-info')][string]$Command = 'memory-benchmark --plan-only')
    $info = [Diagnostics.ProcessStartInfo]::new($cli, "$Command $Arguments")
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = Read-BoundedBenchmarkStream -Reader $process.StandardOutput
        $stderr = Read-BoundedBenchmarkStream -Reader $process.StandardError
        if (-not $process.WaitForExit(15000)) { throw 'Native benchmark planning exceeded its 15-second test deadline.' }
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), 5000)) {
            throw 'Native benchmark planning pipes did not close.'
        }
        return @{ exit_code = $process.ExitCode; stdout = $stdout.GetAwaiter().GetResult(); stderr = $stderr.GetAwaiter().GetResult() }
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
}

foreach ($mode in @('--force-cpu', '--require-gpu')) {
    $result = Invoke-MemoryBenchmarkPlanProbe -Arguments "$mode --workers 4 --inflight 1 --decode-inflight 1"
    $stats = if ($result.exit_code -eq 0) { ConvertFrom-StatsLine -Line $result.stdout.Trim() } else { @{} }
    if ($result.exit_code -ne 0 -or $result.stderr -or $result.stdout -notmatch '^plan_only=true ' -or
        $stats['input_bytes'] -ne '10737418240' -or $stats['workers'] -ne '4' -or
        $stats['inflight_chunks'] -ne '1' -or $stats['codec_workers'] -ne '4' -or
        $stats['decode_inflight_chunks'] -ne '1' -or $stats['decode_codec_workers'] -ne '4' -or
        $stats['compression_mode'] -cne 'standard' -or
        $result.stdout -match 'gpu_used=|seconds=|memory_only=') {
        throw "Native planning must preserve exact geometry without timing or GPU claims: $($result.stderr)"
    }
}
foreach ($case in @(
        @{ args = '--workers 1 --inflight 2'; cause = 'exceeds current host/worker admission' },
        @{ args = '--workers 1 --inflight 1 --decode-inflight 2'; cause = 'decode inflight depth exceeds current admission' },
        @{ args = '--inflight 65'; cause = 'resource limit' },
        @{ args = '--decode-inflight 65'; cause = 'resource limit' },
        @{ args = '--workers 65'; cause = 'resource limit' },
        @{ args = '--size-mib 10239'; cause = 'at least 10240 MiB' },
        @{ args = '--force-cpu --require-gpu'; cause = 'mutually exclusive' },
        @{ args = '--profile Unknown'; cause = 'unknown memory benchmark profile' },
        @{ args = '--neutron-star --force-cpu'; cause = 'excludes --force-cpu' },
        @{ args = '--neutron-star --compression-level 8'; cause = 'requires compression level 9' }
    )) {
    $result = Invoke-MemoryBenchmarkPlanProbe -Arguments $case.args
    if ($result.exit_code -eq 0 -or $result.stdout -match 'plan_only=true|entries=' -or $result.stderr -notmatch $case.cause) {
        throw "Native planning accepted invalid admission or changed its failure cause: $($case.args): $($result.stderr)"
    }
}
$gpuProbe = Invoke-MemoryBenchmarkPlanProbe -Command 'gpu-info' -Arguments ''
$availableLines = @($gpuProbe.stdout -split '\r?\n' | Where-Object { $_ -cmatch '^available=' })
if ($gpuProbe.stderr -or $availableLines.Count -ne 1 -or $availableLines[0] -cnotmatch '^available=(true|false)$') {
    throw 'Planning test received malformed HIP capability evidence.'
}
$gpuAvailable = $availableLines[0] -ceq 'available=true'
$expectedCapabilityExit = if ($gpuAvailable) { 0 } else { 1 }
if ($gpuProbe.exit_code -ne $expectedCapabilityExit) {
    throw 'Planning test received inconsistent HIP capability and exit status.'
}
if ($gpuAvailable) {
    $result = Invoke-MemoryBenchmarkPlanProbe -Arguments '--neutron-star --workers 1 --inflight 1 --decode-inflight 1'
    $stats = if ($result.exit_code -eq 0) { ConvertFrom-StatsLine -Line $result.stdout.Trim() } else { @{} }
    if ($result.exit_code -ne 0 -or $result.stderr -or $stats['compression_mode'] -cne 'neutron_star' -or
        $stats['workers'] -ne '1' -or $stats['inflight_chunks'] -ne '1' -or
        $stats['decode_inflight_chunks'] -ne '1' -or $result.stdout -match 'gpu_used=|seconds=|memory_only=') {
        throw 'Neutron planning must preserve separate mode identity without reporting executed GPU work.'
    }
} else {
    $statusLines = @($gpuProbe.stdout -split '\r?\n' | Where-Object { $_ -cmatch '^status=.+' })
    $result = Invoke-MemoryBenchmarkPlanProbe -Arguments '--neutron-star --workers 1 --inflight 1 --decode-inflight 1'
    if ($statusLines.Count -ne 1 -or $result.exit_code -ne 1 -or
        $result.stdout -match 'plan_only=true|entries=' -or -not $result.stderr.Contains($statusLines[0].Substring(7))) {
        throw 'Neutron planning must reject unavailable HIP with the capability cause and no execution claims.'
    }
}
Write-Output 'memory_benchmark_plan status=passed measured_workloads=0'
