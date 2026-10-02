param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$cli = Join-Path (Split-Path -Parent $PSScriptRoot) "build/$Configuration/superzip_cli.exe"
. (Join-Path $PSScriptRoot 'benchmark_statistics.ps1')

# Purpose: Probe native planning and invalid admission without executing a multi-gigabyte workload.
# Inputs: Arguments are a controlled CLI fixture; each subprocess has bounded lifetime and output.
# Outputs: Returns exit code and complete stdout/stderr, or terminates only the owned process on failure.
function Invoke-MemoryBenchmarkPlanProbe {
    param([string]$Arguments)
    $info = [Diagnostics.ProcessStartInfo]::new($cli, "memory-benchmark --plan-only $Arguments")
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
    if ($result.exit_code -ne 0 -or $result.stderr -or $result.stdout -notmatch '^plan_only=true ' -or
        $result.stdout -notmatch ' input_bytes=10737418240 workers=4 inflight_chunks=1 codec_workers=4 ' -or
        $result.stdout -notmatch ' decode_inflight_chunks=1 decode_codec_workers=4' -or
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
        @{ args = '--profile Unknown'; cause = 'unknown memory benchmark profile' }
    )) {
    $result = Invoke-MemoryBenchmarkPlanProbe -Arguments $case.args
    if ($result.exit_code -eq 0 -or $result.stdout -match 'plan_only=true|entries=' -or $result.stderr -notmatch $case.cause) {
        throw "Native planning accepted invalid admission or changed its failure cause: $($case.args): $($result.stderr)"
    }
}
Write-Output 'memory_benchmark_plan status=passed probes=10 measured_workloads=0'
