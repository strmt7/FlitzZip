$ErrorActionPreference = "Stop"

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
    $source = Join-Path $sourceRoot 'src/codec.cpp'
    New-Item -ItemType Directory -Path (Split-Path -Parent $source) -Force | Out-Null
    Set-Content -LiteralPath $source -Value 'relevant source'
    if (-not (Get-RamBenchmarkSourceDirty -RepositoryRoot $sourceRoot)) {
        throw 'Untracked codec source was missed by native benchmark provenance.'
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
    $record.runs[0].gpu_encode_stage_worker_seconds.dictionary -ne 5.5) {
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
        $stored.runs[0].gpu_encode_stage_worker_seconds.dictionary -ne 5.5) {
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

# Purpose: Substitute deterministic CLI output without launching a benchmark.
# Inputs: Remaining arguments are deliberately ignored; the fixture is one valid stats row.
# Outputs: Emits one stats row and sets a successful native exit status.
function Invoke-FakeBenchmarkCli {
    $global:LASTEXITCODE = 0
    'entries=1 seconds=1 memory_only=true disk_write_bytes=0'
}

$script:cli = "Invoke-FakeBenchmarkCli"
$script:NoResourceCounters = $true
foreach ($showStats in @($false, $true)) {
    $script:ShowOperationStatsEnabled = $showStats
    $records = @(Invoke-SuperZipStat -Arguments @("memory-benchmark") 6>$null)
    if ($records.Count -ne 1 -or $records[0] -isnot [hashtable]) {
        throw "Operation diagnostics corrupted the statistics return stream."
    }
    if ($records[0].seconds -ne "1" -or $null -ne $records[0].cpu_avg_pct -or
        $null -ne $records[0].resource_sample_count) {
        throw "Statistics or unavailable counter semantics changed."
    }
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
Write-Output "benchmark_reporting status=passed"
