$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'fuzz_resources.ps1')

# Purpose: Assert resource rejection without accepting unrelated failures.
# Inputs: Action invokes a tested boundary and Message is a required diagnostic fragment.
# Outputs: Throws unless that action fails with the expected error.
function Assert-ResourceRejection {
    param([scriptblock]$Action, [string]$Message)
    try { & $Action | Out-Null } catch {
        if (-not $_.Exception.Message.Contains($Message)) { throw }
        return
    }
    throw "Resource boundary accepted invalid input: $Message"
}

foreach ($case in @(@(4096, 4096, 2048), @(32768, 8192, 4096), @(8192, 32768, 4096),
        @(65536, 65536, 32768), @(2147483647, 2147483647, 1073741823))) {
    $budget = Resolve-SuperZipFuzzMemoryBudget -AvailableMiB $case[0] -DockerAvailableMiB $case[1]
    if ($budget -ne $case[2]) { throw 'Fuzz admission must preserve headroom on both hosts without a fixed cap.' }
}
foreach ($low in @(0, 2048, 4095)) {
    Assert-ResourceRejection { Resolve-SuperZipFuzzMemoryBudget -AvailableMiB $low -DockerAvailableMiB 65536 } 'Insufficient available RAM'
    Assert-ResourceRejection { Resolve-SuperZipFuzzMemoryBudget -AvailableMiB 65536 -DockerAvailableMiB $low } 'Insufficient available RAM'
}

$script:dockerInfo = [ordered]@{ OSType = 'linux'; MemoryLimit = $true; SwapLimit = $true; MemTotal = 16GB }
$script:dockerCalls = @()
$script:probeOutput = '8192'
$script:infoExit = 0
$script:probeExit = 0

# Purpose: Replace Docker with a deterministic offline resource preflight fixture.
# Inputs: Native-style argv in args and script-scoped response fixtures.
# Outputs: Records argv, returns fixture output, and sets the same native exit-code variable.
function docker {
    $script:dockerCalls += ,@($args)
    if ($args[0] -eq 'info') {
        $global:LASTEXITCODE = $script:infoExit
        return ($script:dockerInfo | ConvertTo-Json -Compress)
    }
    $global:LASTEXITCODE = $script:probeExit
    return $script:probeOutput
}

$budget = Get-SuperZipDockerAvailableMemoryMiB -Image 'fixture@sha256:fixed' -RepoMount 'C:/fixture with spaces'
if ($budget -ne 8192 -or $script:dockerCalls.Count -ne 2) { throw 'Docker preflight must query and probe exactly once.' }
$probeArgs = $script:dockerCalls[1]
foreach ($flag in @('--memory', '--memory-swap')) {
    $index = [Array]::IndexOf([object[]]$probeArgs, $flag)
    if ($index -lt 0 -or $probeArgs[$index + 1] -ne '64m') { throw 'The resource probe must itself be memory-contained.' }
}
if ($probeArgs -notcontains 'C:/fixture with spaces:/src:ro') { throw 'Probe mounts must preserve spaces and read-only ownership.' }
if (@($probeArgs | Where-Object { $_ -match '^--cpu' }).Count -ne 0) { throw 'Fuzz resource admission must not cap CPUs.' }
$script:infoExit = 7
Assert-ResourceRejection { Get-SuperZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'preflight failed with exit code 7'
$script:infoExit = 0
foreach ($case in @(@('OSType', 'windows'), @('MemoryLimit', $false), @('SwapLimit', $false),
        @('MemoryLimit', 'true'), @('MemTotal', 0), @('MemTotal', 'unknown'))) {
    $previous = $script:dockerInfo[$case[0]]
    try {
        $script:dockerInfo[$case[0]] = $case[1]
        Assert-ResourceRejection { Get-SuperZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'requires Linux Docker'
    } finally { $script:dockerInfo[$case[0]] = $previous }
}
$script:probeExit = 9
Assert-ResourceRejection { Get-SuperZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'probe failed with exit code 9'
$script:probeExit = 0
foreach ($invalid in @('', '0', '-1', '8192 warning', '1.5', '99999999999')) {
    $script:probeOutput = $invalid
    Assert-ResourceRejection { Get-SuperZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'valid available-memory counter'
}
$script:probeOutput = '16385'
Assert-ResourceRejection { Get-SuperZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'exceeds the reported host capacity'
Write-Output 'fuzz_resource_tests status=passed'
