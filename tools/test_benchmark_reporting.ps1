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
Write-Output "benchmark_reporting status=passed"
