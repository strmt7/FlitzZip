param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked,
    [switch]$Full,
    [switch]$Json
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

$plan = Get-SuperZipVerificationPlan `
    -ChangedPath $ChangedPath `
    -BaseRef $BaseRef `
    -HeadRef $HeadRef `
    -IncludeUntracked:$IncludeUntracked `
    -SuspectGlobalBug:$Full

if ($Json.IsPresent) {
    $plan | ConvertTo-Json -Depth 8
    return
}

Write-Host "SuperZip verification plan"
Write-Host "Changed paths: $($plan.scope.pathCount)"
foreach ($path in $plan.scope.paths) {
    Write-Host "  - $path"
}
Write-Host "Scope:"
foreach ($name in @(
        "docsOnly",
        "touchesCpp",
        "touchesProductionSource",
        "touchesArchiveParser",
        "touchesSecurityBoundary",
        "touchesGui",
        "touchesPackaging",
        "touchesVerification",
        "fullEscalationRequired")) {
    Write-Host ("  {0}={1}" -f $name, $plan.scope.$name)
}
if ($plan.scope.fullEscalationReasons.Count -gt 0) {
    Write-Host "Escalation reasons:"
    foreach ($reason in $plan.scope.fullEscalationReasons) {
        Write-Host "  - $reason"
    }
}

Write-Host "Required local commands:"
foreach ($command in $plan.requiredLocalCommands) {
    Write-Host "  [$($command.id)] $($command.command)"
    Write-Host "      $($command.reason)"
}
if ($plan.manualLocalCommands.Count -gt 0) {
    Write-Host "Manual local commands:"
    foreach ($command in $plan.manualLocalCommands) {
        Write-Host "  [$($command.id)] $($command.command)"
        Write-Host "      $($command.reason)"
    }
}
if ($plan.postPushWorkflows.Count -gt 0) {
    Write-Host "Relevant post-push workflows to wait for:"
    foreach ($workflow in $plan.postPushWorkflows) {
        Write-Host "  - $workflow"
    }
} else {
    Write-Host "Relevant post-push workflows to wait for: none"
}
Write-Host "Post-push audit required: $($plan.postPushAuditRequired)"
Write-Host "Workflow wait policy:"
Write-Host "  immediateRequired=$($plan.workflowWaitPolicy.immediateRequired)"
Write-Host "  deferAllowed=$($plan.workflowWaitPolicy.deferAllowed)"
Write-Host "  recommendedMode=$($plan.workflowWaitPolicy.recommendedMode)"
if ($plan.workflowWaitPolicy.reasons.Count -gt 0) {
    Write-Host "  reasons:"
    foreach ($reason in $plan.workflowWaitPolicy.reasons) {
        Write-Host "    - $reason"
    }
}
