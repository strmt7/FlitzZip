# Purpose: Run affected offline development contracts through the canonical planner.
# Inputs: Explicit paths or a Git comparison range; PlanOnly requests portable command metadata.
# Outputs: Executes selected contracts or emits their JSON array; never builds or measures the product.
param(
    [string]$BaseRef = '',
    [string]$HeadRef = 'HEAD',
    [string[]]$ChangedPath = @(),
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'superzip_verification.psm1') -Force
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
