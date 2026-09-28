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

$fixtureRun = [pscustomobject]@{
    Lane = 'GPU'; Iteration = 1; BlockSizeKiB = 1024
    MemoryOnly = 'true'; DiskWriteBytes = 0; InputBytes = 10GB; OutputBytes = 171079680; ArchiveBytes = 171102811
    CompressSeconds = 1.0; VerifySeconds = 1.0; ExtractSeconds = 1.0
    CpuAvgPct = $null; CpuPeakPct = $null; GpuAvgPct = $null; GpuPeakPct = $null
    GpuKernelLaunches = 720; GpuKernelMs = $null
    GpuPatternBlocks = 0; GpuPrefixBlocks = 0; GpuDictionaryBlocks = 0; GpuSparsePatternBlocks = 10240
}
$record = ConvertTo-RamBenchmarkRecord -Runs @($fixtureRun) -Commit ('a' * 40) -Dirty $true `
    -BinarySha256 ('B' * 64) -Profile 'SparseRecord' -SizeMiB 10240 -Level 5 -SampleIntervalMs 100
if ($record.schema_version -ne 1 -or $record.source_dirty -ne $true -or
    $record.binary_sha256 -ne ('b' * 64) -or $record.runs.Count -ne 1 -or
    $record.runs[0].output_bytes -ne 171079680 -or
    $record.runs[0].archive_bytes -ne 171102811 -or $null -ne $record.runs[0].gpu_kernel_ms) {
    throw 'RAM benchmark JSON lost provenance, exact size, or unavailable counter semantics.'
}
$jsonPath = Join-Path $env:TEMP ("superzip-benchmark-json-" + [guid]::NewGuid().ToString('N') + '.json')
try {
    Write-BenchmarkJson -Record $record -Path $jsonPath
    $json = Get-Content -LiteralPath $jsonPath -Raw
    $stored = $json | ConvertFrom-Json
    if ($stored.runs[0].output_bytes -ne 171079680 -or
        $stored.runs[0].archive_bytes -ne 171102811 -or $stored.source_dirty -ne $true) {
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
    if ($records[0].seconds -ne "1" -or $null -ne $records[0].cpu_avg_pct) {
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
