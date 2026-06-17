param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked,
    [switch]$Full,
    [switch]$Json
)

$ErrorActionPreference = "Stop"
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

Write-Output "SuperZip verification plan"
Write-Output "Changed paths: $($plan.scope.pathCount)"
foreach ($path in $plan.scope.paths) {
    Write-Output "  - $path"
}
Write-Output "Scope:"
foreach ($name in @(
        "docsOnly",
        "touchesCpp",
        "touchesProductionSource",
        "touchesArchiveParser",
        "touchesSecurityBoundary",
        "touchesGui",
        "touchesPackaging",
        "touchesLintSurface",
        "touchesVerification",
        "fullEscalationRequired")) {
    Write-Output ("  {0}={1}" -f $name, $plan.scope.$name)
}
if ($plan.scope.fullEscalationReasons.Count -gt 0) {
    Write-Output "Escalation reasons:"
    foreach ($reason in $plan.scope.fullEscalationReasons) {
        Write-Output "  - $reason"
    }
}

Write-Output "Required local commands:"
foreach ($command in $plan.requiredLocalCommands) {
    Write-Output "  [$($command.id)] $($command.command)"
    Write-Output "      $($command.reason)"
}
if ($plan.manualLocalCommands.Count -gt 0) {
    Write-Output "Manual local commands:"
    foreach ($command in $plan.manualLocalCommands) {
        Write-Output "  [$($command.id)] $($command.command)"
        Write-Output "      $($command.reason)"
    }
}
if ($plan.postPushWorkflows.Count -gt 0) {
    Write-Output "Relevant post-push workflows to wait for:"
    foreach ($workflow in $plan.postPushWorkflows) {
        Write-Output "  - $workflow"
    }
} else {
    Write-Output "Relevant post-push workflows to wait for: none"
}
if ($plan.longRunningPostPushWorkflows.Count -gt 0) {
    Write-Output "Long-running workflows to observe, not normally wait for:"
    foreach ($workflow in $plan.longRunningPostPushWorkflows) {
        Write-Output "  - $workflow"
    }
} else {
    Write-Output "Long-running workflows to observe: none"
}
Write-Output "Post-push audit required: $($plan.postPushAuditRequired)"
Write-Output "Workflow wait policy:"
Write-Output "  immediateRequired=$($plan.workflowWaitPolicy.immediateRequired)"
Write-Output "  deferAllowed=$($plan.workflowWaitPolicy.deferAllowed)"
Write-Output "  recommendedMode=$($plan.workflowWaitPolicy.recommendedMode)"
Write-Output "  longRunningNormallyDeferred=$($plan.workflowWaitPolicy.longRunningNormallyDeferred)"
if ($plan.workflowWaitPolicy.reasons.Count -gt 0) {
    Write-Output "  reasons:"
    foreach ($reason in $plan.workflowWaitPolicy.reasons) {
        Write-Output "    - $reason"
    }
}
