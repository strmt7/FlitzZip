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
    $budget = Resolve-FlitzZipFuzzMemoryBudget -AvailableMiB $case[0] -DockerAvailableMiB $case[1]
    if ($budget -ne $case[2]) { throw 'Fuzz admission must preserve headroom on both hosts without a fixed cap.' }
}
foreach ($low in @(0, 2048, 4095)) {
    Assert-ResourceRejection { Resolve-FlitzZipFuzzMemoryBudget -AvailableMiB $low -DockerAvailableMiB 65536 } 'Insufficient available RAM'
    Assert-ResourceRejection { Resolve-FlitzZipFuzzMemoryBudget -AvailableMiB 65536 -DockerAvailableMiB $low } 'Insufficient available RAM'
}

$script:dockerInfo = [ordered]@{ OSType = 'linux'; MemoryLimit = $true; SwapLimit = $true; MemTotal = 16GB }
$script:dockerCalls = @()
$script:probeOutput = '8192'
$script:infoExit = 0
$script:probeExit = 0
$script:ownedIds = @()
$script:cleanupQueryExit = 0
$script:inspectProof = ''
$script:removeExit = 0
$script:cancelProbe = $false

# Purpose: Replace Docker with a deterministic offline resource preflight fixture.
# Inputs: Native-style argv in args and script-scoped response fixtures.
# Outputs: Records argv, returns fixture output, and sets the same native exit-code variable.
function docker {
    $script:dockerCalls += ,@($args)
    if ($args[0] -eq 'info') {
        $global:LASTEXITCODE = $script:infoExit
        return ($script:dockerInfo | ConvertTo-Json -Compress)
    }
    if ($args[0] -eq 'ps') { $global:LASTEXITCODE = $script:cleanupQueryExit; return $script:ownedIds }
    if ($args[0] -eq 'inspect') { $global:LASTEXITCODE = 0; return $script:inspectProof }
    if ($args[0] -eq 'rm') {
        $global:LASTEXITCODE = $script:removeExit
        $removedId = $args[-1]
        if ($script:removeExit -eq 0) { $script:ownedIds = @($script:ownedIds | Where-Object { $_ -cne $removedId }) }
        return
    }
    if ($script:cancelProbe) {
        $script:ownedIds = @('a' * 64)
        throw [OperationCanceledException]::new('owned probe cancellation')
    }
    $global:LASTEXITCODE = $script:probeExit
    return $script:probeOutput
}

$budget = Get-FlitzZipDockerAvailableMemoryMiB -Image 'fixture@sha256:fixed' -RepoMount 'C:/fixture with spaces'
if ($budget -ne 8192 -or $script:dockerCalls.Count -ne 5) { throw 'Docker preflight must query/probe once and verify cleanup.' }
$probeArgs = $script:dockerCalls[2]
foreach ($flag in @('--memory', '--memory-swap')) {
    $index = [Array]::IndexOf([object[]]$probeArgs, $flag)
    if ($index -lt 0 -or $probeArgs[$index + 1] -ne '64m') { throw 'The resource probe must itself be memory-contained.' }
}
if ($probeArgs -notcontains 'C:/fixture with spaces:/src:ro') { throw 'Probe mounts must preserve spaces and read-only ownership.' }
if (@($probeArgs | Where-Object { $_ -match '^--cpu' }).Count -ne 0) { throw 'Fuzz resource admission must not cap CPUs.' }
$script:infoExit = 7
Assert-ResourceRejection { Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'preflight failed with exit code 7'
$script:infoExit = 0
foreach ($case in @(@('OSType', 'windows'), @('MemoryLimit', $false), @('SwapLimit', $false),
        @('MemoryLimit', 'true'), @('MemTotal', 0), @('MemTotal', 'unknown'))) {
    $previous = $script:dockerInfo[$case[0]]
    try {
        $script:dockerInfo[$case[0]] = $case[1]
        Assert-ResourceRejection { Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'requires Linux Docker'
    } finally { $script:dockerInfo[$case[0]] = $previous }
}
$script:probeExit = 9
Assert-ResourceRejection { Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'probe failed with exit code 9'
$script:probeExit = 0
foreach ($invalid in @('', '0', '-1', '8192 warning', '1.5', '99999999999')) {
    $script:probeOutput = $invalid
    Assert-ResourceRejection { Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'valid available-memory counter'
}
$script:probeOutput = '16385'
Assert-ResourceRejection { Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture } 'exceeds the reported host capacity'

$owner = [guid]::NewGuid().ToString('N')
$container = 'a' * 64
$script:ownedIds = @($container)
$labelProof = @{ 'io.flitzzip.fuzz-owner' = $owner } | ConvertTo-Json -Compress
$script:inspectProof = "/flitzzip-fuzz-$owner-work|$labelProof"
$script:dockerCalls = @()
Assert-ResourceRejection { Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner -WhatIf } 'cleanup was declined'
if (@($script:dockerCalls | Where-Object { $_[0] -ceq 'rm' }).Count -ne 0) {
    throw 'WhatIf must preserve the container and cannot report cleanup complete.'
}
$script:dockerCalls = @()
Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner
if ($script:ownedIds.Count -ne 0) { throw 'Successful cleanup retained its owned container.' }
$removals = @($script:dockerCalls | Where-Object { $_[0] -ceq 'rm' })
if ($removals.Count -ne 1 -or $removals[0][-1] -cne $container -or
    $removals[0] -notcontains '--force' -or $removals[0] -notcontains '--volumes') {
    throw 'Cleanup must remove only the independently verified complete container ID.'
}
$wrongOwner = @{ 'io.flitzzip.fuzz-owner' = [guid]::NewGuid().ToString('N') } | ConvertTo-Json -Compress
$wrongType = @{ 'io.flitzzip.fuzz-owner' = @($owner) } | ConvertTo-Json -Compress
foreach ($proof in @("/other-name|$labelProof", "/flitzzip-fuzz-$owner-work|{}", '',
        "/flitzzip-fuzz-$owner-work-extra|$labelProof", "/flitzzip-fuzz-$owner-work|[]",
        "/flitzzip-fuzz-$owner-work|$wrongOwner", "/flitzzip-fuzz-$owner-work|$wrongType")) {
    $script:ownedIds = @($container)
    $script:inspectProof = $proof
    $script:dockerCalls = @()
    Assert-ResourceRejection { Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner } 'identity does not match'
    if (@($script:dockerCalls | Where-Object { $_[0] -ceq 'rm' }).Count -ne 0) {
        throw 'Cleanup attempted to remove a container with mismatched ownership.'
    }
}
$script:ownedIds = @('truncated-id')
Assert-ResourceRejection { Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner } 'invalid container ID'
$script:cleanupQueryExit = 7
Assert-ResourceRejection { Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner } 'ownership query failed (7)'
$script:cleanupQueryExit = 0
$script:ownedIds = @($container)
$script:inspectProof = "/flitzzip-fuzz-$owner-probe|$labelProof"
$script:removeExit = 8
Assert-ResourceRejection { Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner } 'removal failed'
if ($script:ownedIds.Count -ne 1) { throw 'Removal failure must retain the unresolved owner.' }
$script:removeExit = 0
$script:dockerCalls = @()
Assert-ResourceRejection {
    Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture -OwnerId $owner
} 'owner is already in use'
if (@($script:dockerCalls | Where-Object { $_[0] -ceq 'rm' -or $_[0] -ceq 'run' }).Count -ne 0) {
    throw 'A duplicate owner must preserve the existing operation without launching or removing containers.'
}
$script:ownedIds = @()
$script:cancelProbe = $true
Assert-ResourceRejection {
    Get-FlitzZipDockerAvailableMemoryMiB -Image fixture -RepoMount fixture -OwnerId $owner
} 'owned probe cancellation'
if ($script:ownedIds.Count -ne 0) { throw 'Probe cancellation left its owned container.' }
$script:cancelProbe = $false
Remove-FlitzZipFuzzOwnedContainer -OwnerId $owner
Write-Output 'fuzz_resource_tests status=passed'
