$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'benchmark_statistics.ps1')

# Load only function definitions, without starting a workload or touching disk fixtures.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "bench.ps1"), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "Benchmark script did not parse." }
$definitions = $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)
foreach ($definition in $definitions) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

$distribution = Measure-BenchmarkDistribution -Values @(1, 2, 3)
if ($distribution.count -ne 3 -or $distribution.mean_seconds -ne 2 -or
    $distribution.sample_std_dev_seconds -ne 1 -or $distribution.relative_std_dev_pct -ne 50 -or
    [math]::Abs($distribution.relative_standard_error_pct - (50 / [math]::Sqrt(3))) -gt 1e-12) {
    throw 'Sample statistics do not use the scientific n-1 variance and independence-assuming RSE formulas.'
}
$tail = Measure-BenchmarkDistribution -Values @(1, 1, 1, 100)
if ($tail.count -ne 4 -or $tail.mean_seconds -ne 25.75 -or $tail.max_seconds -ne 100 -or $tail.median_seconds -ne 1) {
    throw 'A slow observation was discarded or given a different weight.'
}
# Fractional timings must retain variance and the same CV when the time unit changes.
foreach ($scale in @(0.001, 1.0, 1000.0)) {
    $fractional = Measure-BenchmarkDistribution -Values @((4.0 * $scale), (4.1 * $scale), (4.2 * $scale))
    $expectedSd = 0.1 * $scale
    $expectedCv = 100.0 * 0.1 / 4.1
    if ([math]::Abs($fractional.sample_std_dev_seconds - $expectedSd) -gt $expectedSd * 1e-10 -or
        [math]::Abs($fractional.relative_std_dev_pct - $expectedCv) -gt 1e-10 -or
        [math]::Abs($fractional.relative_standard_error_pct - $expectedCv / [math]::Sqrt(3.0)) -gt 1e-10) {
        throw 'Fractional timing variance or unit-independent uncertainty was lost.'
    }
}
if ($null -ne (Measure-BenchmarkDistribution -Values @(1)).sample_std_dev_seconds) {
    throw 'Single-sample uncertainty was fabricated as zero.'
}
$offset = Measure-BenchmarkDistribution -Values @(1000000, 1000001, 1000002)
if ($offset.sample_std_dev_seconds -ne 1) { throw 'Variance lost precision for large offsets.' }
foreach ($invalid in @([double]::NaN, [double]::PositiveInfinity, 0, -1, 1e10)) {
    $rejected = $false
    try { Measure-BenchmarkDistribution -Values @(1, $invalid, 3) | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid timing was silently removed instead of rejecting the sample.' }
}

# Purpose: Supply complete deterministic measurements to test planning without running compression.
# Inputs: Lane, iteration, block size and seconds define one observation; all resource and size fields are controlled.
# Outputs: Returns one fixture with complete scientific timing and evidence fields.
function Get-PlanningRun {
    param([string]$Lane = 'CPU', [int]$Iteration = 1, [int]$BlockSizeKiB = 256, [double]$Seconds = 1)
    [pscustomobject]@{ Lane = $Lane; Iteration = $Iteration; BlockSizeKiB = $BlockSizeKiB
        CompressSeconds = $Seconds; VerifySeconds = $Seconds; ExtractSeconds = $Seconds
        InputBytes = 10GB; OutputBytes = 1024; ArchiveBytes = 4096
        Workers = 32; InflightChunks = 32; CodecWorkers = 1; DecodeInflightChunks = 32; DecodeCodecWorkers = 1
        ResourceSampleCount = 50; GpuSampleCount = 25 }
}
$fractionalPilot = @(foreach ($seconds in @(1.0, 1.1, 1.2)) { Get-PlanningRun -Seconds $seconds })
$fractionalPlan = @(Get-BenchmarkConfirmationPlan -PilotRuns $fractionalPilot -Blocks @(256) -Lanes @('CPU') `
    -MinimumCount 3 -MaximumCount 30 -MinimumSeconds 1 -TargetRsePct 2)
if ($fractionalPlan[0].requested_count -ne 21 -or $fractionalPlan[0].confirmation_count -ne 21 -or
    $fractionalPlan[0].count_capped) {
    throw 'Fractional pilot variance did not increase the fixed confirmation count.'
}
$fractionalEvidence = Get-BenchmarkLaneEvidence -Runs $fractionalPilot -MinimumSeconds 1
if ($fractionalEvidence.issues -notcontains 'high_variability:CompressSeconds') {
    throw 'Fractional noisy measurements were declared stable.'
}
$pilot = @(foreach ($iteration in 1..3) { Get-PlanningRun -Iteration $iteration })
$plan = @(Get-BenchmarkConfirmationPlan -PilotRuns $pilot -Blocks @(256) -Lanes @('CPU') `
    -MinimumCount 3 -MaximumCount 15 -MinimumSeconds 30 -TargetRsePct 2)
if ($plan[0].requested_count -ne 10 -or $plan[0].confirmation_count -ne 10 -or $plan[0].count_capped) {
    throw 'Short workloads did not receive a duration-based fixed confirmation count.'
}
$pilot[1].CompressSeconds = 2
$pilot[2].CompressSeconds = 3
$plan = @(Get-BenchmarkConfirmationPlan -PilotRuns $pilot -Blocks @(256) -Lanes @('CPU') `
    -MinimumCount 3 -MaximumCount 15 -MinimumSeconds 1 -TargetRsePct 2)
if ($plan[0].requested_count -ne 625 -or $plan[0].confirmation_count -ne 15 -or -not $plan[0].count_capped) {
    throw 'Noisy pilot planning failed to preserve the uncapped request and enforce the count ceiling.'
}
$evidence = Get-BenchmarkLaneEvidence -Runs $pilot -MinimumSeconds 1
if ($evidence.issues -notcontains 'high_variability:CompressSeconds') { throw 'Noisy measurements were declared stable.' }
$pilot[2].Workers = 16
$pilot[0].ResourceSampleCount = $null
$pilot[2].ArchiveBytes = 4097
$evidence = Get-BenchmarkLaneEvidence -Runs $pilot -MinimumSeconds 1
foreach ($issue in @('resource_policy_changed:Workers', 'resource_counters_unavailable', 'inconsistent_size:ArchiveBytes')) {
    if ($evidence.issues -notcontains $issue) { throw "Missing measurement diagnostic: $issue" }
}

# Purpose: Substitute prescribed observations and record execution order for scheduler regression tests.
# Inputs: Lane, mode flag, iteration and block size are supplied by the real scheduler.
# Outputs: Returns a complete fixture or throws at the requested negative-control sample.
function Invoke-MemoryBenchmarkLane {
    param([string]$Lane, [string]$ModeFlag, [int]$Iteration, [int]$BlockSizeKiB)
    if ($ModeFlag -ne $(if ($Lane -eq 'CPU') { '--force-cpu' } else { '--require-gpu' })) { throw 'Wrong lane mode.' }
    if ($script:FailPlanningSample -and $Iteration -eq 2) { throw 'Intentional failed read-back fixture.' }
    $run = Get-PlanningRun -Lane $Lane -Iteration $Iteration -BlockSizeKiB $BlockSizeKiB
    if ($script:ChangePlanningGeometry -and $Iteration -eq 2) { $run.InflightChunks -= 1 }
    return $run
}
$script:SkipCpu = $false; $script:SkipGpu = $false; $script:InterRunPauseMs = 0
$script:BenchmarkSampleIdentities = @{}
$journal = Join-Path $env:TEMP ('superzip-sample-journal-' + [guid]::NewGuid().ToString('N') + '.jsonl')
try {
    Write-BenchmarkJournal -Path $journal -Event @{ event = 'protocol' } -Create
    $plans = @(@{ block_size_kib = 256; confirmation_count = 2 }, @{ block_size_kib = 512; confirmation_count = 1 })
    $samples = @(Invoke-BenchmarkPlannedSample -Plans $plans -Stage confirmation -JournalPath $journal 6>$null)
    $order = ($samples | ForEach-Object { "$($_.BlockSizeKiB):$($_.Lane):$($_.Iteration)" }) -join ','
    if ($order -ne '256:CPU:1,256:GPU:1,512:CPU:1,512:GPU:1,256:GPU:2,256:CPU:2') {
        throw 'Scheduler changed a frozen count, dropped a lane, or failed to counterbalance order.'
    }
    $events = @(Get-Content -LiteralPath $journal | ForEach-Object { $_ | ConvertFrom-Json })
    if ($events.Count -ne 7 -or @($events | Where-Object { $_.event -eq 'sample' }).Count -ne 6) {
        throw 'Completed observations were lost from the append-only journal.'
    }
    $threeRoundPlans = @(@{ block_size_kib = 256; confirmation_count = 3 }, @{ block_size_kib = 512; confirmation_count = 3 })
    $threeRoundSamples = @(Invoke-BenchmarkPlannedSample -Plans $threeRoundPlans -Stage confirmation 6>$null)
    $thirdRound = @($threeRoundSamples | Where-Object Iteration -eq 3)
    if ($threeRoundPlans[0].block_size_kib -ne 256 -or $thirdRound[0].BlockSizeKiB -ne 256 -or
        $threeRoundSamples[4].BlockSizeKiB -ne 512 -or $threeRoundSamples.Count -ne 12) {
        throw 'Counterbalancing mutated the prescribed plan or failed to restore odd-round case order.'
    }
    $rejected = $false
    try { Write-BenchmarkJournal -Path $journal -Event @{} -Create } catch { $rejected = $true }
    if (-not $rejected) { throw 'Sample journal replaced existing evidence.' }
    $script:FailPlanningSample = $true
    $rejected = $false
    try { Invoke-BenchmarkPlannedSample -Plans $plans -Stage pilot -JournalPath $journal 6>$null | Out-Null } catch { $rejected = $true }
    $events = @(Get-Content -LiteralPath $journal | ForEach-Object { $_ | ConvertFrom-Json })
    if (-not $rejected -or $events[-1].event -ne 'failure' -or $events[-1].cause -ne 'Intentional failed read-back fixture.') {
        throw 'A failed sample was silently retried or omitted from the journal.'
    }
    $script:FailPlanningSample = $false; $script:ChangePlanningGeometry = $true
    $script:SkipGpu = $true; $script:BenchmarkSampleIdentities = @{}
    $beforeCount = $events.Count
    $rejected = $false
    try {
        Invoke-BenchmarkPlannedSample -Plans @(@{ block_size_kib = 256; confirmation_count = 3 }) `
            -Stage confirmation -JournalPath $journal 6>$null | Out-Null
    } catch { $rejected = $_.Exception.Message -match 'case identity changed' }
    $after = @(Get-Content -LiteralPath $journal | ForEach-Object { $_ | ConvertFrom-Json })
    if (-not $rejected -or $after.Count -ne ($beforeCount + 3) -or $after[-2].run.InflightChunks -ne 31 -or
        $after[-1].event -ne 'failure') {
        throw 'Configuration drift did not abort immediately while retaining the mismatched observation.'
    }
} finally { Remove-Item -LiteralPath $journal -Force }

$script:SizeMiB = 10240
$geometry = @{ plan_only = 'true'; input_bytes = "$(10GB)"; workers = '32'; inflight_chunks = '28'
    codec_workers = '1'; decode_inflight_chunks = '4'; decode_codec_workers = '8' }
Assert-BenchmarkGeometry -Stats $geometry -Expected $geometry -PlanOnly
foreach ($key in @('workers', 'inflight_chunks', 'codec_workers', 'decode_inflight_chunks', 'decode_codec_workers')) {
    foreach ($invalidValue in @($null, 'NaN', '0', '65', '1.5', '2')) {
        $invalid = $geometry.Clone(); $invalid[$key] = $invalidValue
        $rejected = $false
        try { Assert-BenchmarkGeometry -Stats $invalid -Expected $geometry -PlanOnly } catch { $rejected = $true }
        if (-not $rejected) { throw "Admission accepted changed or malformed geometry: $key" }
    }
}
foreach ($key in @('seconds', 'gpu_used')) {
    $invalid = $geometry.Clone(); $invalid[$key] = '0'
    $rejected = $false
    try { Assert-BenchmarkGeometry -Stats $invalid -Expected $geometry -PlanOnly } catch { $rejected = $true }
    if (-not $rejected) { throw 'Planning was treated as measured GPU or timing evidence.' }
}
$identities = @{}
$first = Get-PlanningRun
Assert-BenchmarkSampleIdentity -Run $first -Identities $identities
foreach ($key in @('Workers', 'InflightChunks', 'CodecWorkers', 'DecodeInflightChunks', 'DecodeCodecWorkers',
        'InputBytes', 'OutputBytes', 'ArchiveBytes')) {
    $changed = Get-PlanningRun -Iteration 2
    $changed.$key += 1
    $rejected = $false
    try { Assert-BenchmarkSampleIdentity -Run $changed -Identities $identities } catch { $rejected = $true }
    if (-not $rejected) { throw "Sample identity tracking ignored $key." }
}

foreach ($prefix in @('gpu_', 'gpu_classification_', 'decode_')) {
    $values = Read-BenchmarkWorkerStageSet -Stats @{ "${prefix}allocation_worker_seconds" = '0.125' } `
        -Prefix $prefix -Stages @('allocation') -Lane 'fixture'
    if ($values.Count -ne 1 -or $values['allocation'] -ne 0.125) {
        throw 'CLI stage parsing lost the named production interval.'
    }
    foreach ($invalid in @($null, '', 'NaN', 'Infinity', '-0.1')) {
        $rejected = $false
        try {
            Read-BenchmarkWorkerStageSet -Stats @{ "${prefix}allocation_worker_seconds" = $invalid } `
                -Prefix $prefix -Stages @('allocation') -Lane 'fixture' | Out-Null
        } catch { $rejected = $true }
        if (-not $rejected) { throw 'Invalid CLI stage value was accepted.' }
    }
}

foreach ($iteration in @(1, 2, 3, 4)) {
    $expected = if (($iteration % 2) -eq 1) { 'CPU,GPU' } else { 'GPU,CPU' }
    if ((@(Get-BenchmarkLaneOrder -Iteration $iteration) -join ',') -ne $expected -or
        (@(Get-BenchmarkLaneOrder -Iteration $iteration -SkipCpu) -join ',') -ne 'GPU' -or
        (@(Get-BenchmarkLaneOrder -Iteration $iteration -SkipGpu) -join ',') -ne 'CPU' -or
        @(Get-BenchmarkLaneOrder -Iteration $iteration -SkipCpu -SkipGpu).Count -ne 0) {
        throw 'Benchmark rounds did not alternate enabled lanes deterministically.'
    }
}

$sourceRoot = Join-Path $env:TEMP ("superzip-benchmark-source-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sourceRoot | Out-Null
try {
    & git -C $sourceRoot init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize source-state test repository.' }
    $skill = Join-Path $sourceRoot '.agents/skills/other/SKILL.md'
    New-Item -ItemType Directory -Path (Split-Path -Parent $skill) -Force | Out-Null
    Set-Content -LiteralPath $skill -Value 'unrelated skill'
    if (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot) {
        throw 'Unrelated skill made native benchmark source dirty.'
    }
    $fixtureTools = Join-Path $sourceRoot 'tools'
    New-Item -ItemType Directory -Path $fixtureTools | Out-Null
    $untrackedLock = Join-Path $fixtureTools 'rocm-sdk-lock.json'
    Set-Content -LiteralPath $untrackedLock -Value 'untracked SDK pin'
    if (-not (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot)) {
        throw 'Untracked SDK lock was missed by native benchmark provenance.'
    }
    Remove-Item -LiteralPath $untrackedLock
    $source = Join-Path $sourceRoot 'src/codec.cpp'
    New-Item -ItemType Directory -Path (Split-Path -Parent $source) -Force | Out-Null
    Set-Content -LiteralPath $source -Value 'relevant source'
    if (-not (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot)) {
        throw 'Untracked codec source was missed by native benchmark provenance.'
    }
    # Establish a real clean commit, then exercise each build input in both tracked states.
    $buildInputs = @(
        'version.ps1', 'compile_hip_object.ps1', 'hip_architecture.ps1',
        'rocm-sdk-lock.json', 'rocm_toolchain.ps1', 'bootstrap_rocm_sdk.py',
        'process_environment.ps1', 'cmake_toolchain.ps1',
        'build_parallelism.ps1', 'local_resources.ps1'
    )
    foreach ($buildInput in $buildInputs) {
        Set-Content -LiteralPath (Join-Path $fixtureTools $buildInput) -Value 'original build input'
    }
    & git -C $sourceRoot add -- src tools
    if ($LASTEXITCODE -ne 0) { throw 'Could not stage source-state fixture inputs.' }
    & git -C $sourceRoot -c user.name='SuperZip fixture' -c user.email='fixture@example.invalid' `
        -c commit.gpgsign=false -c core.hooksPath=$fixtureTools commit --quiet -m 'Fixture baseline'
    if ($LASTEXITCODE -ne 0) { throw 'Could not commit source-state fixture inputs.' }
    if (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot) {
        throw 'Committed build inputs or unrelated untracked skills made benchmark source dirty.'
    }
    foreach ($buildInput in $buildInputs) {
        $inputPath = Join-Path $fixtureTools $buildInput
        Set-Content -LiteralPath $inputPath -Value 'modified build input'
        if (-not (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot)) {
            throw "Unstaged build input was missed by benchmark provenance: $buildInput"
        }
        & git -C $sourceRoot add -- "tools/$buildInput"
        if ($LASTEXITCODE -ne 0) { throw "Could not stage fixture input: $buildInput" }
        if (-not (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot)) {
            throw "Staged build input was missed by benchmark provenance: $buildInput"
        }
        Set-Content -LiteralPath $inputPath -Value 'original build input'
        & git -C $sourceRoot add -- "tools/$buildInput"
        if ($LASTEXITCODE -ne 0) { throw "Could not restore fixture input contents: $buildInput" }
        if (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot) {
            throw "Restored fixture input still appeared dirty: $buildInput"
        }
    }
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($sourceRoot)
    $tempPrefix = [IO.Path]::GetFullPath($env:TEMP).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing source-state fixture cleanup outside the temporary directory.'
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}

$fixtureRun = [pscustomobject]@{
    Lane = 'GPU'; Iteration = 1; BlockSizeKiB = 1024
    MemoryOnly = 'true'; DiskWriteBytes = 0; InputBytes = 10GB; OutputBytes = 171079680; ArchiveBytes = 171102811
    CompressSeconds = 1.0; VerifySeconds = 1.0; ExtractSeconds = 1.0
    SourceGenerationWorkerSeconds = 4.25; CodecEncodeWorkerSeconds = 9.5
    GpuEncodeStages = [ordered]@{
        readiness = 0.5; analysis = 0.1; classification = 1.25; prefix = 0.75
        sparse = 0.2; dictionary = 5.5; publication = 0.3
    }
    OwnedDecodeStages = [ordered]@{ allocation = 0.25; materialization = 2.5; crc = 0.75 }
    GpuClassificationStages = [ordered]@{ allocation = 0.05; upload = 0.25; crc = 0.75; validation = 0.15 }
    CpuAvgPct = $null; CpuPeakPct = $null; GpuAvgPct = $null; GpuPeakPct = $null
    ResourceSampleCount = 25; GpuSampleCount = 23; ResourceSampleMeanIntervalMs = 102.5
    GpuKernelLaunches = 720; GpuKernelMs = $null
    GpuHostPinnedAllocMiB = 512; GpuHostPinnedOutputMiB = 10240; DecodeInflightChunks = 4; DecodeCodecWorkers = 8
    GpuPatternBlocks = 0; GpuPrefixBlocks = 0; GpuDictionaryBlocks = 0; GpuSparsePatternBlocks = 10240
}
$record = ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
    -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 `
    -HipRuntimeVersion '10.0.3679.0'
if ($record.runs[0].gpu_host_pinned_allocation_bytes -ne 512MB -or
    $record.runs[0].gpu_host_pinned_output_bytes -ne 10GB -or
    $record.runs[0].decode_inflight_chunks -ne 4 -or $record.runs[0].decode_codec_workers -ne 8) {
    throw 'Pinned allocation, reused output, and decode admission telemetry were conflated or lost.'
}
if ($record.schema_version -ne 2 -or $record.gpu_utilization_metric -ne 'process_busiest_engine_pct' -or
    $record.lane_order -ne 'alternating_when_both' -or $record.inter_run_pause_ms -ne 250 -or
    $record.source_dirty -ne $true -or $record.hip_runtime_version -ne '10.0.3679.0' -or
    $record.binary_sha256 -ne ('b' * 64) -or $record.runs.Count -ne 1 -or
    $record.runs[0].output_bytes -ne 171079680 -or
    $record.runs[0].archive_bytes -ne 171102811 -or $null -ne $record.runs[0].gpu_kernel_ms -or
    $record.runs[0].source_generation_worker_seconds -ne 4.25 -or
    $record.runs[0].codec_encode_worker_seconds -ne 9.5 -or
    $record.runs[0].gpu_sample_count -ne 23 -or
    $record.runs[0].resource_sample_mean_interval_ms -ne 102.5 -or
    $record.runs[0].gpu_encode_stage_worker_seconds.dictionary -ne 5.5 -or
    $record.runs[0].gpu_classification_stage_worker_seconds.upload -ne 0.25 -or
    $record.runs[0].gpu_classification_stage_worker_seconds.crc -ne 0.75 -or
    $record.runs[0].owned_decode_stage_worker_seconds.allocation -ne 0.25 -or
    $record.runs[0].owned_decode_stage_worker_seconds.materialization -ne 2.5 -or
    $record.runs[0].owned_decode_stage_worker_seconds.crc -ne 0.75) {
    throw 'RAM benchmark JSON lost provenance, exact size, or unavailable counter semantics.'
}
$jsonPath = Join-Path $env:TEMP ("superzip-benchmark-json-" + [guid]::NewGuid().ToString('N') + '.json')
try {
    Write-BenchmarkJson -Record $record -Path $jsonPath
    $json = Get-Content -LiteralPath $jsonPath -Raw
    $stored = $json | ConvertFrom-Json
    if ($stored.schema_version -ne 2 -or $stored.gpu_utilization_metric -ne 'process_busiest_engine_pct' -or
        $stored.lane_order -ne 'alternating_when_both' -or $stored.inter_run_pause_ms -ne 250 -or
        $stored.runs[0].output_bytes -ne 171079680 -or
        $stored.hip_runtime_version -ne '10.0.3679.0' -or
        $stored.runs[0].archive_bytes -ne 171102811 -or $stored.source_dirty -ne $true -or
        $stored.runs[0].source_generation_worker_seconds -ne 4.25 -or
        $stored.runs[0].codec_encode_worker_seconds -ne 9.5 -or
        $stored.runs[0].resource_sample_count -ne 25 -or
        $stored.runs[0].gpu_sample_count -ne 23 -or
        $stored.runs[0].gpu_encode_stage_worker_seconds.dictionary -ne 5.5 -or
        $stored.runs[0].gpu_classification_stage_worker_seconds.upload -ne 0.25 -or
        $stored.runs[0].gpu_classification_stage_worker_seconds.crc -ne 0.75 -or
        $stored.runs[0].owned_decode_stage_worker_seconds.allocation -ne 0.25 -or
        $stored.runs[0].owned_decode_stage_worker_seconds.materialization -ne 2.5 -or
        $stored.runs[0].owned_decode_stage_worker_seconds.crc -ne 0.75) {
        throw 'Serialized RAM benchmark JSON differs from the record.'
    }
    if ($record.runs[0].gpu_kernel_launches -isnot [int64] -or
        $record.runs[0].gpu_sparse_pattern_blocks -isnot [int64] -or
        $json -notmatch '"gpu_kernel_launches"\s*:\s*720\b' -or
        $json -notmatch '"gpu_sparse_pattern_blocks"\s*:\s*10240\b' -or
        $stored.runs[0].gpu_kernel_launches -ne 720 -or
        $stored.runs[0].gpu_sparse_pattern_blocks -ne 10240) {
        throw 'Telemetry counters lost exact integer JSON representation.'
    }
    $collisionRejected = $false
    try { Write-BenchmarkJson -Record $record -Path $jsonPath } catch { $collisionRejected = $true }
    if (-not $collisionRejected) { throw 'Benchmark JSON overwrote existing evidence.' }
} finally {
    Remove-Item -LiteralPath $jsonPath -Force -ErrorAction SilentlyContinue
}
foreach ($missing in @($null, '', 'unavailable')) {
    if ($null -ne (ConvertTo-HipRuntimeVersionEvidence -Value $missing)) {
        throw 'Missing runtime metadata was synthesized.'
    }
}
foreach ($invalidVersion in @('10.0.65536.0', '010.0.3679.0', '10.0.3679.0/path', '10.0.3679', '10.0.3679.0 ', "10.0.3679.0`n")) {
    $rejected = $false
    try { ConvertTo-HipRuntimeVersionEvidence -Value $invalidVersion | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Malformed runtime identity was accepted.' }
}
foreach ($invalid in @(-1.0, [double]::NaN, [double]::PositiveInfinity)) {
    $fixtureRun.CodecEncodeWorkerSeconds = $invalid
    $invalidRejected = $false
    try {
        ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
            -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
    } catch { $invalidRejected = $true }
    if (-not $invalidRejected) { throw 'Invalid codec worker time entered the RAM-only evidence record.' }
}
$fixtureRun.CodecEncodeWorkerSeconds = 9.5
foreach ($invalid in @(-1.0, [double]::NaN, [double]::PositiveInfinity)) {
    $fixtureRun.GpuEncodeStages['dictionary'] = $invalid
    $invalidRejected = $false
    try {
        ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
            -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
    } catch { $invalidRejected = $true }
    if (-not $invalidRejected) { throw 'Invalid GPU encode stage time entered the RAM-only evidence record.' }
}
$fixtureRun.GpuEncodeStages['dictionary'] = 5.5

foreach ($stage in @('allocation', 'upload', 'crc', 'validation')) {
    $saved = $fixtureRun.GpuClassificationStages[$stage]
    foreach ($invalid in @($null, -1.0, [double]::NaN, [double]::PositiveInfinity)) {
        $fixtureRun.GpuClassificationStages[$stage] = $invalid
        $rejected = $false
        try {
            ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
                -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
        } catch { $rejected = $true }
        if (-not $rejected) { throw "Invalid $stage nested classification time was accepted." }
    }
    $fixtureRun.GpuClassificationStages[$stage] = $saved
    $fixtureRun.GpuClassificationStages.Remove($stage)
    $rejected = $false
    try {
        ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
            -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
    } catch { $rejected = $true }
    if (-not $rejected) { throw "Missing $stage nested classification time was accepted." }
    $fixtureRun.GpuClassificationStages[$stage] = $saved
}
$fixtureRun.GpuClassificationStages['unknown'] = 0.0
$rejected = $false
try {
    ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
        -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
} catch { $rejected = $true }
if (-not $rejected) { throw 'Unknown nested classification stage was accepted.' }
$fixtureRun.GpuClassificationStages.Remove('unknown')

foreach ($stage in @('allocation', 'materialization', 'crc')) {
    $saved = $fixtureRun.OwnedDecodeStages[$stage]
    foreach ($invalid in @($null, -1.0, [double]::NaN, [double]::PositiveInfinity)) {
        $fixtureRun.OwnedDecodeStages[$stage] = $invalid
        $rejected = $false
        try {
            ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
                -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
        } catch { $rejected = $true }
        if (-not $rejected) { throw "Invalid $stage owned-decode stage time was accepted." }
    }
    $fixtureRun.OwnedDecodeStages[$stage] = $saved
    $fixtureRun.OwnedDecodeStages.Remove($stage)
    $rejected = $false
    try {
        ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
            -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
    } catch { $rejected = $true }
    if (-not $rejected) { throw "Missing $stage owned-decode stage was accepted." }
    $fixtureRun.OwnedDecodeStages[$stage] = $saved
}
$fixtureRun.OwnedDecodeStages['unknown'] = 0.0
$rejected = $false
try {
    ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
        -BinarySha256 ('B' * 64) -Profile 'Mixed' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
} catch { $rejected = $true }
if (-not $rejected) { throw 'Unknown owned-decode stage was accepted.' }
$fixtureRun.OwnedDecodeStages.Remove('unknown')
$fixtureRun.GpuSampleCount = 26
$invalidRejected = $false
try {
    ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
        -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
} catch { $invalidRejected = $true }
if (-not $invalidRejected) { throw 'GPU sample count exceeded total resource samples.' }
$fixtureRun.GpuSampleCount = 23
foreach ($field in @('GpuAvgPct', 'GpuPeakPct')) {
    foreach ($invalid in @([double]::NaN, [double]::PositiveInfinity, -1.0, 101.0)) {
        $fixtureRun.$field = $invalid
        $invalidRejected = $false
        try {
            ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
                -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
        } catch { $invalidRejected = $true }
        if (-not $invalidRejected) { throw 'An invalid GPU percentage entered the evidence record.' }
    }
    $fixtureRun.$field = $null
}
$fixtureRun.MemoryOnly = 'false'
$invalidRejected = $false
try {
    ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
        -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
} catch { $invalidRejected = $true }
if (-not $invalidRejected) { throw 'Filesystem benchmark data entered the RAM-only evidence record.' }
$fixtureRun.MemoryOnly = 'true'
$fixtureRun.ArchiveBytes = $fixtureRun.OutputBytes
$invalidRejected = $false
try {
    ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
        -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100 | Out-Null
} catch { $invalidRejected = $true }
if (-not $invalidRejected) { throw 'Payload-only bytes were accepted as complete archive bytes.' }
foreach ($invalid in @([double]::NaN, 1.5, -1, 9007199254740992)) {
    $counterRejected = $false
    try { ConvertTo-ExactBenchmarkCounter $invalid | Out-Null } catch { $counterRejected = $true }
    if (-not $counterRejected) { throw 'Invalid benchmark telemetry counter was accepted.' }
}

if ($null -ne (ConvertTo-MiBPerSecond -Value $null)) {
    throw "Missing byte counters must stay unavailable, not become zero."
}
if ((ConvertTo-MiBPerSecond -Value 1MB) -ne 1) {
    throw "MiB/s conversion is incorrect."
}

class FakeGpuCategory {
    [string[]]$Instances
    [string[]] GetInstanceNames() { return $this.Instances }
}
class FakeGpuCounter {
    [single]$Value
    [bool]$Disposed
    [bool]$Fail
    [single] NextValue() {
        if ($this.Fail) { throw 'disappearing test counter' }
        return $this.Value
    }
    [void] Dispose() { $this.Disposed = $true }
}
$ownedCounter = [FakeGpuCounter]::new()
$ownedCounter.Value = 12.5
$otherCounter = [FakeGpuCounter]::new()
$otherCounter.Value = 99.0
$category = [FakeGpuCategory]::new()
$category.Instances = @('pid_42_engine', 'pid_420_engine')
$sampler = @{ Category = $category; Counters = @{ pid_42_engine = $ownedCounter; pid_420_engine = $otherCounter } }
if ((Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -ne 12.5 -or
    -not $otherCounter.Disposed -or $sampler.Counters.ContainsKey('pid_420_engine')) {
    throw 'Persistent GPU sampler included another process or retained a stale engine.'
}
$failingCounter = [FakeGpuCounter]::new()
$failingCounter.Fail = $true
$sampler.Counters['pid_42_failed'] = $failingCounter
$category.Instances = @('pid_42_engine', 'pid_42_failed')
if ((Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -ne 12.5 -or
    -not $failingCounter.Disposed -or $sampler.Counters.ContainsKey('pid_42_failed')) {
    throw 'A disappearing GPU engine suppressed valid samples.'
}
$category.Instances = @()
if ($null -ne (Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -or -not $ownedCounter.Disposed) {
    throw 'Persistent GPU sampler fabricated a value after engines disappeared.'
}
Close-GpuResourceSampler -Sampler $sampler
$engineCounters = @{}
$engineNames = @('pid_42_compute', 'pid_42_copy', 'pid_42_other_gpu')
foreach ($name in $engineNames) { $engineCounters[$name] = [FakeGpuCounter]::new() }
$engineCounters['pid_42_compute'].Value = 80.0
$engineCounters['pid_42_copy'].Value = 70.0
$engineCounters['pid_42_other_gpu'].Value = 90.0
$category.Instances = $engineNames
$sampler = @{ Category = $category; Counters = $engineCounters }
if ((Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -ne 90.0) {
    throw 'GPU percentage must select the busiest process engine, not sum parallel engines or GPUs.'
}
foreach ($invalid in @([single]::NaN, [single]::PositiveInfinity, [single]::NegativeInfinity, -1.0, 101.0)) {
    $engineCounters['pid_42_other_gpu'].Value = $invalid
    if ((Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -ne 80.0) {
        throw 'Invalid GPU counters distorted the busiest-engine measurement.'
    }
}
foreach ($counter in $engineCounters.Values) { $counter.Value = [single]::NaN }
if ($null -ne (Get-ProcessGpuSample -Sampler $sampler -ProcessId 42)) {
    throw 'An entirely invalid GPU sample must remain unavailable.'
}
foreach ($counter in $engineCounters.Values) { $counter.Value = 0.0 }
if ((Get-ProcessGpuSample -Sampler $sampler -ProcessId 42) -ne 0.0) {
    throw 'A valid idle GPU sample must remain zero rather than unavailable.'
}
Close-GpuResourceSampler -Sampler $sampler
$resource = Measure-ResourceSample -Samples @(
    [pscustomobject]@{ Cpu = 20.0; Gpu = $null; ElapsedIntervalMs = 100.0; DiskActive = $null; DiskReadBytesPerSec = $null; DiskWriteBytesPerSec = $null },
    [pscustomobject]@{ Cpu = 40.0; Gpu = 12.5; ElapsedIntervalMs = 120.0; DiskActive = $null; DiskReadBytesPerSec = $null; DiskWriteBytesPerSec = $null }
)
if ($resource.resource_sample_count -ne 2 -or $resource.gpu_sample_count -ne 1 -or
    $resource.resource_sample_mean_interval_ms -ne 110.0) {
    throw 'Resource sampler lost actual cadence or valid GPU sample count.'
}

$script:cli = (Get-Command powershell -CommandType Application).Source
$script:NoResourceCounters = $true
$script:RunTimeoutSeconds = 5
$script:SampleIntervalMs = 50
foreach ($showStats in @($false, $true)) {
    $script:ShowOperationStatsEnabled = $showStats
    $records = @(Invoke-SuperZipStat -Arguments @('-NoProfile', '-Command',
        "Write-Output 'entries=1 seconds=1 memory_only=true disk_write_bytes=0'") 6>$null)
    if ($records.Count -ne 1 -or $records[0] -isnot [hashtable]) {
        throw "Operation diagnostics corrupted the statistics return stream."
    }
    if ($records[0].seconds -ne "1" -or $null -ne $records[0].cpu_avg_pct -or
        $null -ne $records[0].resource_sample_count) {
        throw "Statistics or unavailable counter semantics changed."
    }
    $line = 'plan_only=true input_bytes=10737418240 workers=32 inflight_chunks=25 codec_workers=1 decode_inflight_chunks=4 decode_codec_workers=8'
    $plans = @(Invoke-SuperZipStat -Arguments @('-NoProfile', '-Command', "Write-Output '$line'") 6>$null)
    if ($plans.Count -ne 1 -or $plans[0] -isnot [hashtable] -or $plans[0].plan_only -ne 'true' -or
        $plans[0].ContainsKey('seconds') -or $plans[0].ContainsKey('gpu_used')) {
        throw 'Planning output failed the complete subprocess-reader/parser path or fabricated measurement fields.'
    }
}
$script:RunTimeoutSeconds = 1
foreach ($command in @('Start-Sleep -Seconds 10', "[Console]::Write('x' * 70000); Start-Sleep -Seconds 10")) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $cause = $null
    try { Invoke-SuperZipStat -Arguments @('-NoProfile', '-Command', $command) 6>$null | Out-Null }
    catch { $cause = $_.Exception.Message }
    if (-not $cause -or $cause -notmatch 'deadline|output' -or $clock.Elapsed.TotalSeconds -gt 8) {
        throw "Benchmark subprocess timeout/output containment failed: $cause"
    }
}
$script:RunTimeoutSeconds = 5
$validMemoryStat = @{ memory_only = 'true'; input_bytes = "$(10GB)"; output_bytes = '1024'; archive_bytes = '4096'
    disk_write_bytes = '0'; block_size_bytes = '262144'; workers = '32'; inflight_chunks = '32'; codec_workers = '1'
    decode_inflight_chunks = '32'; decode_codec_workers = '1'; compress_seconds = '1'; verify_seconds = '1'; extract_seconds = '1' }
Assert-MemoryBenchmarkStat -Stats $validMemoryStat -ExpectedInputBytes 10GB -BlockSizeKiB 256
foreach ($key in $validMemoryStat.Keys) {
    $invalid = $validMemoryStat.Clone()
    $invalid.Remove($key)
    $rejected = $false
    try { Assert-MemoryBenchmarkStat -Stats $invalid -ExpectedInputBytes 10GB -BlockSizeKiB 256 } catch { $rejected = $true }
    if (-not $rejected) { throw "Missing $key was silently cast to a valid benchmark value." }
}
foreach ($file in @('gpu_proof.ps1', 'gpu_diagnostic.ps1', 'transfer_diagnostics.ps1')) {
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot $file), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) { throw "$file did not parse." }
    foreach ($definition in $ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst]
        }, $false)) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
}

$validGpu = @{
    gpu_used = 'true'; gpu_kernel_launches = '2'; gpu_kernel_ms = '1.25'
    gpu_h2d_bytes = '4096'; gpu_device_allocation_bytes = '8192'
    gpu_pattern_blocks = '1'; gpu_prefix_blocks = '1'; gpu_dictionary_blocks = '1'; gpu_sparse_pattern_blocks = '1'
}
Assert-GpuBackendStat -Stats $validGpu -Label 'finite fixture'
Assert-GpuProofStat -Stats $validGpu -Label 'finite fixture'
$sparseOnly = $validGpu.Clone()
$sparseOnly.gpu_pattern_blocks = '0'
$sparseOnly.gpu_prefix_blocks = '0'
$sparseOnly.gpu_dictionary_blocks = '0'
Assert-GpuBackendStat -Stats $sparseOnly -Label 'sparse-only fixture' -RequireNativeCompressedBlocks $true
Assert-GpuProofStat -Stats $sparseOnly -Label 'sparse-only fixture' -RequireNativeCompressedBlocks $true
foreach ($invalid in @('NaN', 'nan', 'Infinity', '-Infinity')) {
    $stats = $validGpu.Clone()
    $stats.gpu_kernel_ms = $invalid
    if ($null -ne (Get-StatsNumber -Stats $stats -Key 'gpu_kernel_ms')) {
        throw 'Non-finite timing must remain unavailable.'
    }
    if ($invalid -in @('NaN', 'nan')) {
        Assert-GpuBackendStat -Stats $stats -Label 'unavailable event fixture' 3>$null
    }
    $checks = @(
        { Assert-GpuProofStat -Stats $stats -Label 'invalid fixture' },
        { Get-SuperZipStatNumber -Stats $stats -Name 'gpu_kernel_ms' }
    )
    if ($invalid -notin @('NaN', 'nan')) {
        $checks += { Assert-GpuBackendStat -Stats $stats -Label 'invalid fixture' }
    }
    foreach ($check in $checks) {
        $rejected = $false
        try { & $check | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'A GPU evidence consumer accepted non-finite timing.' }
    }
}
$diagnostic = @{
    diagnostic_kernel_launches = '2'; diagnostic_kernel_ms = '1.25'
    diagnostic_h2d_bytes = '4096'; diagnostic_d2h_bytes = '128'
    diagnostic_device_allocation_bytes = '8192'; diagnostic_checksum = '42'
}
Assert-GpuDiagnosticStat -Stats $diagnostic
foreach ($invalid in @('NaN', 'nan', 'Infinity', '-Infinity', '0', '-1')) {
    $diagnostic.diagnostic_kernel_ms = $invalid
    $rejected = $false
    try { Assert-GpuDiagnosticStat -Stats $diagnostic } catch { $rejected = $true }
    if (-not $rejected) { throw 'A diagnostic accepted invalid timing.' }
}
$byteStats = @{ measurement_protocol = 'bytewise-regenerated-v2'; validated_bytes = 10GB
    workers = 32; validation_worker_limit = 32
    validation_seconds = 2.5; wall_seconds = 5.5 }
Assert-BytewiseBenchmarkValidation -Stats $byteStats -ExpectedInputBytes 10GB
foreach ($mutation in @('measurement_protocol', 'validated_bytes', 'validation_seconds', 'wall_seconds',
        'workers', 'validation_worker_limit')) {
    $invalid = $byteStats.Clone()
    $invalid[$mutation] = $(switch ($mutation) {
        measurement_protocol { 'crc-only-historical' }; validated_bytes { 10GB - 1 }
        validation_seconds { [double]::NaN }; wall_seconds { 1.0 }
        workers { 0 }; validation_worker_limit { 33 }
    })
    $rejected = $false
    try { Assert-BytewiseBenchmarkValidation -Stats $invalid -ExpectedInputBytes 10GB } catch { $rejected = $true }
    if (-not $rejected) { throw "Bytewise integrity evidence accepted invalid $mutation." }
}
$fixtureRun.ArchiveBytes = 171102811
$fixtureRun | Add-Member -NotePropertyMembers @{
    MeasurementProtocol = 'bytewise-regenerated-v2'; ValidatedBytes = 10GB; ValidationSeconds = 2.5; WallSeconds = 5.5
    Workers = 32; ValidationWorkerLimit = 32
}
$byteRun = ConvertTo-RamBenchmarkObservation -Run $fixtureRun -SizeMiB 10240
if ($byteRun.measurement_protocol -ne 'bytewise-regenerated-v2' -or $byteRun.validated_bytes -ne 10GB -or
    $byteRun.validation_worker_limit -ne $fixtureRun.Workers -or
    $byteRun.validation_seconds -ne 2.5 -or $byteRun.wall_seconds -ne 5.5) {
    throw 'Bytewise validation evidence was lost during observation serialization.'
}
# Complete observation lifetimes include native validation and controller costs; frozen counts never shrink to fit.
$wallPilots = @()
foreach ($lane in @('CPU', 'GPU')) {
    foreach ($seconds in $(if ($lane -eq 'CPU') { @(10.0, 12.0, 11.0) } else { @(8.0, 9.0, 8.5) })) {
        $wallPilots += [pscustomobject]@{ Lane = $lane; BlockSizeKiB = 256; ObservationWallSeconds = $seconds }
    }
}
$wallPlans = @(@{ block_size_kib = 256; confirmation_count = 6 })
$wallBudget = Get-BenchmarkConfirmationWallBudget -PilotRuns $wallPilots -Plans $wallPlans `
    -Lanes @('CPU', 'GPU') -RemainingSeconds 129.0 -PauseSeconds 0.25
if ($wallBudget.estimated_seconds -ne 129.0 -or -not $wallBudget.fits_in_remaining_time) {
    throw 'Confirmation wall planning omitted a lane, validation lifetime, prescribed count or pause.'
}
$tooShort = Get-BenchmarkConfirmationWallBudget -PilotRuns $wallPilots -Plans $wallPlans `
    -Lanes @('CPU', 'GPU') -RemainingSeconds 128.999 -PauseSeconds 0.25
if ($tooShort.fits_in_remaining_time -or $wallPlans[0].confirmation_count -ne 6) {
    throw 'The confirmation wall planner changed a frozen count or admitted an insufficient budget.'
}
foreach ($invalidRemaining in @(0, -1, [double]::NaN, [double]::PositiveInfinity)) {
    $rejected = $false
    try {
        Get-BenchmarkConfirmationWallBudget -PilotRuns $wallPilots -Plans $wallPlans `
            -Lanes @('CPU', 'GPU') -RemainingSeconds $invalidRemaining -PauseSeconds 0.25 | Out-Null
    } catch { $rejected = $true }
    if (-not $rejected) { throw 'An invalid remaining wall budget was accepted.' }
}
foreach ($invalidCount in @(0, 3.5, 65)) {
    $rejected = $false
    try {
        Get-BenchmarkConfirmationWallBudget -PilotRuns $wallPilots `
            -Plans @(@{ block_size_kib = 256; confirmation_count = $invalidCount }) `
            -Lanes @('CPU') -RemainingSeconds 1000 -PauseSeconds 0 | Out-Null
    } catch { $rejected = $true }
    if (-not $rejected) { throw 'An invalid frozen confirmation count was accepted.' }
}
$rejected = $false
try {
    Get-BenchmarkConfirmationWallBudget -PilotRuns @($wallPilots | Where-Object Lane -eq 'CPU') -Plans $wallPlans `
        -Lanes @('CPU', 'GPU') -RemainingSeconds 1000 -PauseSeconds 0 | Out-Null
} catch { $rejected = $true }
if (-not $rejected) { throw 'A missing pilot lane silently reduced the wall budget.' }

# Independent temporary bytes exercise mutation detection; they are not executable or performance fixtures.
$artifactRoot = Join-Path $env:TEMP ('superzip-artifact-state-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $artifactRoot | Out-Null
$artifactSource = Join-Path $artifactRoot 'src/codec.cpp'
$artifactBinary = Join-Path $artifactRoot 'build/Release/fixture.exe'
$artifactRuntime = Join-Path $artifactRoot 'build/Release/fixture.dll'
$originalStatFunction = (Get-Item Function:\Invoke-SuperZipStat).ScriptBlock
try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $artifactSource), (Split-Path -Parent $artifactBinary) | Out-Null
    [IO.File]::WriteAllText($artifactSource, 'source bytes')
    [IO.File]::WriteAllText($artifactBinary, 'CLI bytes')
    [IO.File]::WriteAllText($artifactRuntime, 'runtime bytes')
    & git -C $artifactRoot init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Artifact fixture Git initialization failed.' }
    & git -C $artifactRoot add -- src/codec.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Artifact fixture source staging failed.' }
    & git -C $artifactRoot -c user.name='SuperZip Tests' -c user.email='tests@example.invalid' `
        -c commit.gpgsign=false -c core.hooksPath=.git/no-hooks commit --quiet -m 'Artifact fixture source'
    if ($LASTEXITCODE -ne 0) { throw 'Artifact fixture commit failed.' }
    $frozen = Get-RamBenchmarkArtifactState -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary
    if ($frozen.source_dirty -or $frozen.binary_dependencies_sha256.Count -ne 1 -or
        ($frozen | ConvertTo-Json -Depth 4).Contains($artifactRoot)) { throw 'Artifact snapshot is dirty, incomplete or leaks a private path.' }
    Assert-RamBenchmarkArtifactState -Expected $frozen -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary
    foreach ($mutationPath in @($artifactSource, $artifactBinary, $artifactRuntime)) {
        $originalBytes = [IO.File]::ReadAllBytes($mutationPath)
        [IO.File]::WriteAllText($mutationPath, 'mutated bytes')
        $rejected = $false
        try { Assert-RamBenchmarkArtifactState -Expected $frozen -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary }
        catch { $rejected = $_.Exception.Message -match 'changed; journal retained' }
        if (-not $rejected) { throw 'An observed source or artifact mutation was missed.' }
        if ($mutationPath -eq $artifactSource) {
            $dirtyStart = Get-RamBenchmarkArtifactState -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary
        }
        [IO.File]::WriteAllBytes($mutationPath, $originalBytes)
        Assert-RamBenchmarkArtifactState -Expected $frozen -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary
    }
    $rejected = $false
    try { Assert-RamBenchmarkArtifactState -Expected $dirtyStart -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'A dirty starting source was relabeled clean.' }
    Set-Item Function:\Invoke-SuperZipStat -Value {
        param([string[]]$Arguments)
        if ($Arguments.Count -ne 1 -or $Arguments[0] -ne 'unit-fixture') { throw 'The observation arguments changed.' }
        [IO.File]::WriteAllText($artifactBinary, 'changed during observation')
        return @{ seconds = 0.5 }
    }
    $rejected = $false
    $artifactJournal = Join-Path $artifactRoot 'observation.jsonl'
    Write-BenchmarkJournal -Path $artifactJournal -Create -Event @{ event = 'protocol' }
    try {
        Invoke-FrozenMemoryBenchmark -Arguments @('unit-fixture') -Expected $frozen `
            -RepositoryRoot $artifactRoot -BinaryPath $artifactBinary -JournalPath $artifactJournal `
            -JournalContext @{ lane = 'CPU'; block_size_kib = 256; iteration = 1 } | Out-Null
    } catch { $rejected = $_.Exception.Message -match 'changed; journal retained' }
    if (-not $rejected) { throw 'The post-observation artifact check did not execute.' }
    $artifactEvents = @(Get-Content -LiteralPath $artifactJournal | ForEach-Object { $_ | ConvertFrom-Json })
    $rawOperations = @($artifactEvents | Where-Object event -eq 'raw_operation')
    if ($rawOperations.Count -ne 1 -or $rawOperations[0].stats.seconds -ne 0.5 -or
        $rawOperations[0].lane -ne 'CPU' -or $rawOperations[0].block_size_kib -ne 256 -or
        @($artifactEvents | Where-Object event -eq 'sample').Count -ne 0) {
        throw 'Returned native evidence was lost or relabeled valid after a failed artifact check.'
    }
} finally {
    Set-Item Function:\Invoke-SuperZipStat -Value $originalStatFunction
    $resolvedArtifactRoot = [IO.Path]::GetFullPath($artifactRoot)
    $artifactTempPrefix = [IO.Path]::GetFullPath($env:TEMP).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedArtifactRoot.StartsWith($artifactTempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing artifact fixture cleanup outside the temporary directory.'
    }
    Remove-Item -LiteralPath $resolvedArtifactRoot -Recurse -Force
}
Write-Output "benchmark_reporting status=passed"
