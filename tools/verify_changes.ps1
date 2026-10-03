param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked,
    [switch]$Full,
    [ValidateSet('intermediate', 'final')][string]$Checkpoint = 'final',
    [switch]$IncludeManual,
    [switch]$NoAutoEscalate,
    [switch]$PlanOnly
)

$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force
if ($NoAutoEscalate.IsPresent) {
    Write-Verbose 'Automatic escalation is disabled by default; -NoAutoEscalate remains accepted for compatibility.'
}

# Purpose: Run a list of command descriptors in order.
# Inputs: `Commands` is produced by the SuperZip verification planner.
# Outputs: Throws on the first failed command and records executed ids in `Executed`.
function Invoke-VerificationCommandList {
    param(
        [object[]]$Commands,
        [Parameter(Mandatory = $true)]$Executed
    )

    foreach ($command in @($Commands)) {
        Invoke-SuperZipVerificationCommand -Command $command
        [void]$Executed.Add([string]$command.id)
    }
}

$plan = Get-SuperZipVerificationPlan `
    -ChangedPath $ChangedPath `
    -BaseRef $BaseRef `
    -HeadRef $HeadRef `
    -IncludeUntracked:$IncludeUntracked `
    -SuspectGlobalBug:$Full `
    -Checkpoint $Checkpoint

if ($PlanOnly.IsPresent) {
    $plan | ConvertTo-Json -Depth 8
    return
}

$executed = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
try {
    Invoke-VerificationCommandList -Commands $plan.requiredLocalCommands -Executed $executed
    if ($IncludeManual.IsPresent) {
        Invoke-VerificationCommandList -Commands $plan.manualLocalCommands -Executed $executed
    }
} catch {
    Write-Warning "Verification stopped at the first failure. Diagnose the changed mechanism before widening the plan."
    Write-Warning $_.ScriptStackTrace
    throw
}

Write-Output "Targeted verification passed. Commands executed: $($executed.Count)."
