param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked,
    [switch]$Full,
    [switch]$IncludeManual,
    [switch]$NoAutoEscalate,
    [switch]$PlanOnly
)

$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

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

# Purpose: Filter commands that have already completed in the targeted phase.
# Inputs: `Commands` is the full command list and `Executed` contains completed command ids.
# Outputs: Returns commands whose ids are not already executed.
function Select-UnexecutedCommand {
    param(
        [object[]]$Commands,
        [Parameter(Mandatory = $true)]$Executed
    )

    $remaining = @()
    foreach ($command in @($Commands)) {
        if (-not $Executed.Contains([string]$command.id)) {
            $remaining += $command
        }
    }
    return $remaining
}

$plan = Get-SuperZipVerificationPlan `
    -ChangedPath $ChangedPath `
    -BaseRef $BaseRef `
    -HeadRef $HeadRef `
    -IncludeUntracked:$IncludeUntracked `
    -SuspectGlobalBug:$Full

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
    if ($NoAutoEscalate.IsPresent -or $Full.IsPresent) {
        throw
    }
    Write-Warning "Targeted verification failed: $($_.Exception.Message)"
    Write-Warning "Automatically escalating to the full verification profile because a larger bug may be present."
    $fullPlan = Get-SuperZipVerificationPlan `
        -ChangedPath $ChangedPath `
        -BaseRef $BaseRef `
        -HeadRef $HeadRef `
        -IncludeUntracked:$IncludeUntracked `
        -SuspectGlobalBug
    $remaining = Select-UnexecutedCommand -Commands $fullPlan.requiredLocalCommands -Executed $executed
    Invoke-VerificationCommandList -Commands $remaining -Executed $executed
    if ($IncludeManual.IsPresent) {
        $manualRemaining = Select-UnexecutedCommand -Commands $fullPlan.manualLocalCommands -Executed $executed
        Invoke-VerificationCommandList -Commands $manualRemaining -Executed $executed
    }
    throw "Targeted verification failed and full escalation completed; original failure still requires remediation."
}

Write-Host "Targeted verification passed. Commands executed: $($executed.Count)."
