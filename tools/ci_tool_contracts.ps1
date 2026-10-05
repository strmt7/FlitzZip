# Purpose: Run affected offline development contracts through the canonical planner.
# Inputs: Explicit paths or a Git comparison range; PlanOnly requests portable command metadata.
# Outputs: Executes selected contracts or emits their JSON array; never builds or measures the product.
param(
    [string]$BaseRef = '',
    [string]$HeadRef = 'HEAD',
    [string[]]$ChangedPath = @(),
    [switch]$InitialPush,
    [string]$DefaultBranch = '',
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'superzip_verification.psm1') -Force

# Purpose: Select a new branch's changes against its default-branch ancestor, without scanning inherited files again.
# Inputs: Checkout root, GitHub's default branch, and initial-push head. Outputs: Changed paths; fails on missing history.
function Get-SuperZipInitialPushPath {
    param([string]$Root, [string]$DefaultBranch, [string]$HeadRef = 'HEAD')
    if (-not $DefaultBranch -or $DefaultBranch.StartsWith('-')) { throw 'Missing or invalid default branch.' }
    $reference = "refs/remotes/origin/$DefaultBranch"
    & git -C $Root check-ref-format $reference
    if ($LASTEXITCODE -ne 0) { throw 'Invalid default-branch reference.' }
    $base = @(& git -C $Root merge-base $HeadRef $reference)
    if ($LASTEXITCODE -ne 0 -or $base.Count -ne 1 -or $base[0] -notmatch '^[0-9a-f]{40}$') {
        throw 'Cannot establish initial-push ancestry; comparison history is required.'
    }
    $paths = @(& git -C $Root diff --name-only --diff-filter=ACDMRTUX $base[0] $HeadRef)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate initial-push changed paths.' }
    return $paths
}

if ($InitialPush.IsPresent) {
    if ($BaseRef -or $ChangedPath.Count) { throw 'Initial push cannot also supply a comparison range or paths.' }
    $ChangedPath = @(Get-SuperZipInitialPushPath -Root (Split-Path -Parent $PSScriptRoot) -DefaultBranch $DefaultBranch -HeadRef $HeadRef)
    if ($ChangedPath.Count -eq 0) {
        if ($PlanOnly.IsPresent) { Write-Output '[]' } else { Write-Output 'Initial branch contains no changes relative to its default-branch ancestor.' }
        return
    }
}
$plan = Get-SuperZipVerificationPlan -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef -Checkpoint intermediate
# The dedicated lint/security lanes own their policy scans. This lane runs
# actual offline mechanism contracts, never product builds or measurements.
$contracts = @($plan.requiredLocalCommands | Where-Object { Test-SuperZipToolContractCommand -Command $_ })
if ($PlanOnly.IsPresent) {
    ConvertTo-Json -InputObject $contracts -Depth 5
    return
}
Write-Output "Tool contract plan: $($contracts.id -join ', ')"
foreach ($command in $contracts) {
    Invoke-SuperZipVerificationCommand -Command $command
}
Write-Output "Tool contracts passed. Commands executed: $($contracts.Count)."
