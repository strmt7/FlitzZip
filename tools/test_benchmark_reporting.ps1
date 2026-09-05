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
    gpu_pattern_blocks = '1'; gpu_prefix_blocks = '1'
}
Assert-GpuBackendStat -Stats $validGpu -Label 'finite fixture'
Assert-GpuProofStat -Stats $validGpu -Label 'finite fixture'
foreach ($invalid in @('NaN', 'nan', 'Infinity', '-Infinity')) {
    $stats = $validGpu.Clone()
    $stats.gpu_kernel_ms = $invalid
    if ($null -ne (Get-StatsNumber -Stats $stats -Key 'gpu_kernel_ms')) {
        throw 'Non-finite timing must remain unavailable.'
    }
    foreach ($check in @(
            { Assert-GpuBackendStat -Stats $stats -Label 'invalid fixture' },
            { Assert-GpuProofStat -Stats $stats -Label 'invalid fixture' },
            { Get-SuperZipStatNumber -Stats $stats -Name 'gpu_kernel_ms' }
        )) {
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
