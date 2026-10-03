# Scientific descriptive statistics and pilot-based sample planning for RAM benchmarks.
# No measured observation is trimmed, winsorized, or excluded as an outlier.
. (Join-Path $PSScriptRoot 'benchmark_corpus.ps1')

# Purpose: Drain an owned subprocess pipe asynchronously while bounding retained text and allocation growth.
# Inputs: Reader is one redirected stdout/stderr stream; the character ceiling is 65536 per pipe.
# Outputs: Returns a Task of complete text or a faulted task on excess output; no output is silently truncated.
function Read-BoundedBenchmarkStream {
    param([Parameter(Mandatory = $true)][IO.TextReader]$Reader)
    if (-not ([Management.Automation.PSTypeName]'SuperZipBoundedTextReader').Type) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Threading.Tasks;
public static class SuperZipBoundedTextReader {
    // Purpose: Read one pipe within a fixed text ceiling.
    // Inputs: reader is owned by the caller; maximumCharacters limits retained UTF-16 characters.
    // Outputs: Returns complete text; throws on excess output or an I/O error.
    public static async Task<string> ReadAsync(TextReader reader, int maximumCharacters) {
        var buffer = new char[4096];
        var text = new StringBuilder();
        int count;
        while ((count = await reader.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false)) > 0) {
            if (text.Length > maximumCharacters - count) throw new InvalidDataException("Benchmark output limit exceeded.");
            text.Append(buffer, 0, count);
        }
        return text.ToString();
    }
}
'@
    }
    return [SuperZipBoundedTextReader]::ReadAsync($Reader, 65536)
}

# Purpose: Reject absent or malformed product statistics before numeric casts could turn them into zero.
# Inputs: Stats is untrusted CLI text; expected bytes and block size identify the prescribed case.
# Outputs: Returns normally only for exact RAM-only geometry and positive finite operation timings; otherwise throws.
function Assert-MemoryBenchmarkStat {
    param([Collections.IDictionary]$Stats, [int64]$ExpectedInputBytes, [int]$BlockSizeKiB)
    foreach ($key in @('compress_seconds', 'verify_seconds', 'extract_seconds')) {
        $value = Get-StatsNumber -Stats $Stats -Key $key
        if ($null -eq $value -or $value -le 0) { throw "Missing or invalid benchmark timing: $key" }
    }
    foreach ($key in @('input_bytes', 'output_bytes', 'archive_bytes', 'disk_write_bytes', 'block_size_bytes',
            'workers', 'inflight_chunks', 'codec_workers', 'decode_inflight_chunks', 'decode_codec_workers')) {
        $value = Get-StatsNumber -Stats $Stats -Key $key
        if ($null -eq $value -or $null -eq (ConvertTo-ExactBenchmarkCounter $value)) { throw "Missing benchmark counter: $key" }
        if ($key -in @('workers', 'inflight_chunks', 'codec_workers', 'decode_inflight_chunks', 'decode_codec_workers') -and
            ($value -lt 1 -or $value -gt 64)) { throw "Invalid benchmark resource policy: $key" }
    }
    if ($Stats['memory_only'] -ne 'true' -or [double]$Stats['disk_write_bytes'] -ne 0 -or
        [double]$Stats['input_bytes'] -ne $ExpectedInputBytes -or [double]$Stats['block_size_bytes'] -ne ($BlockSizeKiB * 1KB) -or
        [double]$Stats['archive_bytes'] -le [double]$Stats['output_bytes']) { throw 'Benchmark case identity or RAM-only evidence differs from the prescribed workload.' }
}

# Purpose: Build identical planning and measurement arguments with optional exact queue depths.
# Inputs: Backend/block identify the case; the admitted corpus or generated settings supply input; Geometry freezes admission.
# Outputs: Returns CLI arguments without changing workload, effort or worker policy between stages.
function Get-MemoryBenchmarkArgument {
    param([string]$ModeFlag, [int]$BlockSizeKiB, [AllowNull()][Collections.IDictionary]$Geometry, [int]$RequestedDepth = 0)
    $source = if ($null -ne $script:BenchmarkCorpus) {
        @('--source-file', $script:BenchmarkCorpus.path, '--source-sha256', $script:BenchmarkCorpus.source_sha256)
    } else { @('--size-mib', "$SizeMiB", '--profile', $WorkloadProfile) }
    $arguments = @('memory-benchmark') + $source + @($ModeFlag,
        '--workers', "$script:BenchmarkWorkerCount", '--block-size-kib', "$BlockSizeKiB", '--compression-level', "$CompressionLevel")
    if ($null -ne $Geometry) {
        $arguments += @('--inflight', "$($Geometry.inflight_chunks)", '--decode-inflight', "$($Geometry.decode_inflight_chunks)")
    } elseif ($RequestedDepth -gt 0) {
        $arguments += @('--inflight', "$RequestedDepth")
    }
    return $arguments
}

# Purpose: Reject malformed admission plans and changes in a prescribed execution geometry.
# Inputs: Stats is CLI planning or measurement output; Expected is the fixed case configuration.
# Outputs: Requires exact source identity, input bytes and five bounded integer resource settings; metadata never proves timing.
function Assert-BenchmarkGeometry {
    param([Collections.IDictionary]$Stats, [Collections.IDictionary]$Expected, [switch]$PlanOnly)
    if ($PlanOnly -and ($Stats['plan_only'] -ne 'true' -or $Stats.Contains('gpu_used') -or $Stats.Contains('seconds'))) {
        throw 'Admission planning must not masquerade as measured timing or GPU evidence.'
    }
    if ((Get-StatsNumber -Stats $Stats -Key 'input_bytes') -ne (Get-BenchmarkInputByteCount $SizeMiB)) { throw 'Admission plan input size differs.' }
    Assert-BenchmarkCorpusStat -Stats $Stats -PlanOnly:$PlanOnly
    foreach ($key in @('workers', 'inflight_chunks', 'codec_workers', 'decode_inflight_chunks', 'decode_codec_workers')) {
        $value = Get-StatsNumber -Stats $Stats -Key $key
        if ($null -eq $value -or $value -lt 1 -or $value -gt 64 -or $value -ne [math]::Floor($value) -or
            $value -ne (Get-StatsNumber -Stats $Expected -Key $key)) {
            throw "Frozen benchmark geometry differs or is invalid: $key"
        }
    }
}

# Purpose: Stop an experiment at its first cross-sample configuration or exact-size mismatch.
# Inputs: Run is an already journaled observation; Identities retains each case baseline across both stages.
# Outputs: Stores the first identity or throws on any later mismatch, without filtering measured samples.
function Assert-BenchmarkSampleIdentity {
    param($Run, [Collections.IDictionary]$Identities)
    if ($null -eq $Identities) { throw 'Benchmark sample identity tracking was not initialized.' }
    $case = "$($Run.Lane):$($Run.BlockSizeKiB)"
    $fields = @('Workers', 'InflightChunks', 'CodecWorkers', 'DecodeInflightChunks', 'DecodeCodecWorkers',
        'InputBytes', 'OutputBytes', 'ArchiveBytes', 'MeasurementProtocol', 'DataSource', 'SourceSha256')
    if (-not $Identities.Contains($case)) {
        $identity = @{}
        foreach ($field in $fields) { $identity[$field] = $Run.$field }
        $Identities[$case] = $identity
    } else {
        foreach ($field in $fields) {
            if ($Run.$field -ne $Identities[$case][$field]) {
                throw "Benchmark case identity changed: ${case}:$field; entire experiment aborted, observations retained."
            }
        }
    }
}

# Purpose: Describe every positive timing observation without filtering or a confidence claim.
# Inputs: Values contains finite positive seconds; each observation receives equal weight.
# Outputs: Returns count, mean, median, range, sample SD, CV, and an independence-assuming RSE diagnostic.
function Measure-BenchmarkDistribution {
    param([Parameter(Mandatory = $true)][double[]]$Values)
    if ($Values.Count -eq 0) { throw 'Timing observations are required.' }
    $mean = 0.0
    $m2 = 0.0
    $count = 0
    foreach ($value in $Values) {
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0 -or $value -gt 1e9) {
            throw 'Timing observations must be finite positive seconds within the supported bound.'
        }
        ++$count
        $delta = $value - $mean
        $mean += $delta / $count
        $m2 += $delta * ($value - $mean)
    }
    $sorted = @($Values | Sort-Object)
    $middle = [int][math]::Floor($count / 2)
    $median = if ($count % 2) { $sorted[$middle] } else { ($sorted[$middle - 1] + $sorted[$middle]) / 2 }
    $sd = if ($count -gt 1) { [math]::Sqrt([math]::Max(0.0, $m2) / ($count - 1)) } else { $null }
    $cv = if ($null -ne $sd) { 100 * $sd / $mean } else { $null }
    [ordered]@{
        count = $count; mean_seconds = $mean; median_seconds = $median
        min_seconds = $sorted[0]; max_seconds = $sorted[-1]; sample_std_dev_seconds = $sd
        relative_std_dev_pct = $cv
        relative_standard_error_pct = if ($null -ne $cv) { $cv / [math]::Sqrt($count) } else { $null }
        measured_seconds = $mean * $count
    }
}

# Purpose: Diagnose all phase timings and resource/size consistency for one lane.
# Inputs: Runs are complete lane observations; minimum time and variability limits are descriptive safeguards.
# Outputs: Returns distributions and explicit issues; no observations or slow tails are removed.
function Get-BenchmarkLaneEvidence {
    param([Parameter(Mandatory = $true)][object[]]$Runs,
        [double]$MinimumSeconds = 30, [double]$MaxCvPct = 5, [double]$TargetRsePct = 2)
    $issues = [Collections.Generic.List[string]]::new()
    $metrics = [ordered]@{}
    foreach ($phase in @('CompressSeconds', 'VerifySeconds', 'ExtractSeconds', 'TotalSeconds')) {
        $values = @($Runs | ForEach-Object {
            if ($phase -eq 'TotalSeconds') { $_.CompressSeconds + $_.VerifySeconds + $_.ExtractSeconds }
            elseif ($null -eq $_.$phase) { throw "Missing $phase timing observation." }
            else { $_.$phase }
        })
        $metrics[$phase] = Measure-BenchmarkDistribution -Values $values
    }
    if ($Runs.Count -lt 3) { $issues.Add('insufficient_samples') }
    if ($metrics.TotalSeconds.measured_seconds -lt $MinimumSeconds) { $issues.Add('insufficient_measured_time') }
    foreach ($phase in $metrics.Keys) {
        if ($null -ne $metrics[$phase].relative_std_dev_pct -and $metrics[$phase].relative_std_dev_pct -gt $MaxCvPct) {
            $issues.Add("high_variability:$phase")
        }
        if ($null -ne $metrics[$phase].relative_standard_error_pct -and
            $metrics[$phase].relative_standard_error_pct -gt $TargetRsePct) { $issues.Add("planning_precision_not_met:$phase") }
    }
    foreach ($field in @('InputBytes', 'OutputBytes', 'ArchiveBytes')) {
        if (@($Runs | Select-Object -ExpandProperty $field -Unique).Count -ne 1) { $issues.Add("inconsistent_size:$field") }
    }
    foreach ($field in @('Workers', 'InflightChunks', 'CodecWorkers', 'DecodeInflightChunks', 'DecodeCodecWorkers')) {
        $values = @($Runs | ForEach-Object { $_.$field })
        if (@($values | Where-Object { $null -eq $_ -or $_ -lt 1 -or $_ -gt 64 }).Count -gt 0) {
            $issues.Add("missing_or_invalid_resource_policy:$field")
        } elseif (@($values | Sort-Object -Unique).Count -ne 1) { $issues.Add("resource_policy_changed:$field") }
    }
    if (@($Runs | Where-Object { $null -eq $_.ResourceSampleCount -or $_.ResourceSampleCount -lt 1 }).Count -gt 0 -or
        ($Runs[0].Lane -eq 'GPU' -and @($Runs | Where-Object { $null -eq $_.GpuSampleCount -or $_.GpuSampleCount -lt 1 }).Count -gt 0)) {
        $issues.Add('resource_counters_unavailable')
    }
    [ordered]@{ lane = $Runs[0].Lane; sample_count = $Runs.Count; metrics = $metrics; issues = @($issues.ToArray()) }
}

# Purpose: Freeze a paired confirmation count using only the separate pilot sample.
# Inputs: PilotRuns, enabled lanes, requested count, count ceiling, duration floor and assumed independent RSE target.
# Outputs: Returns one immutable plan per block size; capped requests are recorded rather than declared converged.
function Get-BenchmarkConfirmationPlan {
    param([object[]]$PilotRuns, [int[]]$Blocks, [string[]]$Lanes,
        [int]$MinimumCount, [int]$MaximumCount, [double]$MinimumSeconds, [double]$TargetRsePct)
    foreach ($block in $Blocks) {
        $requested = [math]::Max(3, $MinimumCount)
        foreach ($lane in $Lanes) {
            $runs = @($PilotRuns | Where-Object { $_.BlockSizeKiB -eq $block -and $_.Lane -eq $lane })
            $evidence = Get-BenchmarkLaneEvidence -Runs $runs
            $requested = [math]::Max($requested, [math]::Ceiling($MinimumSeconds / $evidence.metrics.TotalSeconds.mean_seconds))
            foreach ($metric in $evidence.metrics.Values) {
                $requested = [math]::Max($requested, [math]::Ceiling([math]::Pow($metric.relative_std_dev_pct / $TargetRsePct, 2)))
            }
        }
        [ordered]@{ block_size_kib = $block; requested_count = [int]$requested
            confirmation_count = [int][math]::Min($MaximumCount, $requested)
            count_capped = ($requested -gt $MaximumCount); stopping_rule = 'count_fixed_before_confirmation' }
    }
}

# Purpose: Estimate the frozen confirmation workload from complete pilot observation lifetimes, including validation.
# Inputs: PilotRuns carry measured controller wall seconds; Plans fix counts, Lanes select cases, and remaining/pause seconds are finite budgets.
# Outputs: Returns an auditable maximum-observed-time estimate and fit decision, not a confidence or hard upper bound.
function Get-BenchmarkConfirmationWallBudget {
    param([object[]]$PilotRuns, [object[]]$Plans, [string[]]$Lanes, [double]$RemainingSeconds, [double]$PauseSeconds)
    if ([double]::IsNaN($RemainingSeconds) -or [double]::IsInfinity($RemainingSeconds) -or $RemainingSeconds -le 0 -or
        [double]::IsNaN($PauseSeconds) -or [double]::IsInfinity($PauseSeconds) -or $PauseSeconds -lt 0 -or
        -not $Plans.Count -or -not $Lanes.Count) { throw 'Invalid confirmation wall budget inputs.' }
    $estimated = 0.0
    foreach ($plan in $Plans) {
        $count = [double]$plan.confirmation_count
        if ([double]::IsNaN($count) -or [double]::IsInfinity($count) -or $count -lt 1 -or $count -gt 64 -or
            $count -ne [math]::Floor($count)) { throw 'Invalid frozen confirmation count.' }
        foreach ($lane in $Lanes) {
            $runs = @($PilotRuns | Where-Object { $_.BlockSizeKiB -eq $plan.block_size_kib -and $_.Lane -eq $lane })
            if (-not $runs.Count) { throw 'Confirmation wall budget is missing a pilot case.' }
            $seconds = @($runs | ForEach-Object { $_.ObservationWallSeconds })
            foreach ($value in $seconds) {
                if ($null -eq $value -or [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                    $value -le 0) { throw 'Invalid pilot observation wall time.' }
            }
            $estimated += $count * (($seconds | Measure-Object -Maximum).Maximum + $PauseSeconds)
        }
    }
    return [ordered]@{ method = 'maximum_pilot_observation_wall_plus_pause_times_frozen_counts'
        estimated_seconds = $estimated; remaining_seconds = $RemainingSeconds
        fits_in_remaining_time = ($estimated -le $RemainingSeconds)
        estimate_limit = 'Observed maxima are not upper bounds; runtime deadlines still apply.' }
}

# Purpose: Append auditable observations or predeclared plans without replacing existing evidence.
# Inputs: Path is an optional new JSONL journal, Event is bounded metadata, and Create reserves it exclusively.
# Outputs: Flushes one UTF-8 line per event; throws on collision or write failure.
function Write-BenchmarkJournal {
    param([string]$Path, [Parameter(Mandatory = $true)]$Event, [switch]$Create)
    if (-not $Path) { return }
    $fullPath = [IO.Path]::GetFullPath($Path)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fullPath)) | Out-Null
    $mode = if ($Create) { [IO.FileMode]::CreateNew } else { [IO.FileMode]::Append }
    $stream = [IO.FileStream]::new($fullPath, $mode, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        try { $writer.WriteLine(($Event | ConvertTo-Json -Depth 8 -Compress)) } finally { $writer.Dispose() }
    } finally { $stream.Dispose() }
}

# Purpose: Collect a prescribed sample without changing its count in response to observed confirmation timings.
# Inputs: Plans fix each case count; Stage separates pilot and confirmation; lanes alternate and block order reverses.
# Outputs: Returns all observations with complete controller wall times; journals completed samples and terminal failures.
function Invoke-BenchmarkPlannedSample {
    param([object[]]$Plans, [string]$Stage, [string]$JournalPath)
    $runs = [Collections.Generic.List[object]]::new()
    $maximum = ($Plans | ForEach-Object { $_.confirmation_count } | Measure-Object -Maximum).Maximum
    for ($iteration = 1; $iteration -le $maximum; ++$iteration) {
        $orderedPlans = [object[]]$Plans.Clone()
        if ($iteration % 2 -eq 0) { [array]::Reverse($orderedPlans) }
        foreach ($plan in $orderedPlans) {
            if ($iteration -gt $plan.confirmation_count) { continue }
            foreach ($lane in @(Get-BenchmarkLaneOrder -Iteration $iteration -SkipCpu:$SkipCpu -SkipGpu:$SkipGpu)) {
                if ($InterRunPauseMs -gt 0) { Start-Sleep -Milliseconds $InterRunPauseMs }
                Write-Information "benchmark_sample stage=$Stage block=$($plan.block_size_kib) lane=$lane iteration=$iteration/$($plan.confirmation_count)" -InformationAction Continue
                try {
                    if ($null -ne $script:BenchmarkSuiteClock -and
                        $script:BenchmarkSuiteClock.Elapsed.TotalSeconds -ge $SuiteTimeoutSeconds) {
                        throw "Benchmark suite exceeded ${SuiteTimeoutSeconds}s deadline; incomplete experiment retained."
                    }
                    $flag = if ($lane -eq 'CPU') { '--force-cpu' } else { '--require-gpu' }
                    $observationClock = [Diagnostics.Stopwatch]::StartNew()
                    $run = Invoke-MemoryBenchmarkLane -Lane $lane -ModeFlag $flag -Iteration $iteration -BlockSizeKiB $plan.block_size_kib
                    Add-Member -InputObject $run -NotePropertyName ObservationWallSeconds `
                        -NotePropertyValue $observationClock.Elapsed.TotalSeconds -Force
                    Write-BenchmarkJournal -Path $JournalPath -Event @{ event = 'sample'; stage = $Stage; run = $run; recorded_utc = [datetime]::UtcNow.ToString('o') }
                    Assert-BenchmarkSampleIdentity -Run $run -Identities $script:BenchmarkSampleIdentities
                    $runs.Add($run)
                } catch {
                    Write-BenchmarkJournal -Path $JournalPath -Event @{ event = 'failure'; stage = $Stage; lane = $lane
                        block_size_kib = $plan.block_size_kib; iteration = $iteration; cause = $_.Exception.Message }
                    throw
                }
            }
        }
    }
    return $runs.ToArray()
}
