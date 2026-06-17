$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

# Purpose: Throw a readable assertion failure for verification selector tests.
# Inputs: `Condition` is the assertion result and `Message` describes the expected behavior.
# Outputs: Throws when `Condition` is false.
function Assert-Selector {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "verification selector assertion failed: $Message"
    }
}

# Purpose: Return command ids from a verification plan.
# Inputs: `Plan` is produced by `Get-SuperZipVerificationPlan`.
# Outputs: Returns local required command ids.
function Get-RequiredCommandId {
    param([Parameter(Mandatory = $true)]$Plan)

    return @($Plan.requiredLocalCommands | ForEach-Object { $_.id })
}

# Purpose: Test that a plan contains a required local command id.
# Inputs: `Plan` is a verification plan and `Id` is the expected command id.
# Outputs: Returns true when the command is present.
function Test-RequiredCommand {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Id
    )

    return @($Plan.requiredLocalCommands | Where-Object { $_.id -eq $Id }).Count -gt 0
}

# Purpose: Test that a plan selects a post-push workflow.
# Inputs: `Plan` is a verification plan and `Name` is the expected workflow name.
# Outputs: Returns true when the workflow is selected.
function Test-Workflow {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return @($Plan.postPushWorkflows | Where-Object { $_ -eq $Name }).Count -gt 0
}

# Purpose: Invoke the workflow waiter in a no-network mode for selector smoke coverage.
# Inputs: `Arguments` are passed to `wait_relevant_workflows.ps1`.
# Outputs: Returns the native process exit code.
function Invoke-WaiterSmoke {
    param([string[]]$Arguments)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "powershell"
    $startInfo.WorkingDirectory = (Split-Path -Parent $PSScriptRoot)
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $allArguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (Join-Path $PSScriptRoot "wait_relevant_workflows.ps1")
    ) + @($Arguments)
    $startInfo.Arguments = (@($allArguments) | ForEach-Object { '"' + ([string]$_ -replace '"', '\"') + '"' }) -join " "

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    return $process.ExitCode
}

$docsPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/targeted-verification.md")
Assert-Selector $docsPlan.scope.docsOnly "docs-only changes must be classified as docsOnly"
Assert-Selector (-not $docsPlan.scope.fullEscalationRequired) "docs-only changes must not escalate"
Assert-Selector ((Get-RequiredCommandId -Plan $docsPlan).Count -eq 1) "docs-only changes must require only changed hygiene"
$base64Index = [Array]::IndexOf([object[]]$docsPlan.requiredLocalCommands[0].arguments, "-ChangedPathBase64")
Assert-Selector ($base64Index -ge 0) "hygiene command must use deterministic Base64 JSON path handoff"
$decodedPaths = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($docsPlan.requiredLocalCommands[0].arguments[$base64Index + 1])) | ConvertFrom-Json
Assert-Selector (@($decodedPaths)[0] -eq "docs/targeted-verification.md") "hygiene command must receive the exact planned changed path"
Assert-Selector ($docsPlan.postPushWorkflows.Count -eq 0) "docs-only changes must not wait for workflows"
Assert-Selector ($docsPlan.workflowWaitPolicy.recommendedMode -eq "none") "docs-only changes must not recommend workflow waiting"

$sourcePlan = Get-SuperZipVerificationPlan -ChangedPath @("src/core/checksum.cpp")
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "release-build") "C++ source changes must build"
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "unit-tests") "C++ source changes must run tests"
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "changed-refactor-audit") "C++ source changes must run changed refactor audit"
Assert-Selector (Test-Workflow -Plan $sourcePlan -Name "windows-ci") "C++ source changes must wait for windows-ci"
Assert-Selector ($sourcePlan.manualLocalCommands.Count -eq 0) "ordinary non-performance source changes must not require manual benchmark sweeps"
Assert-Selector $sourcePlan.workflowWaitPolicy.deferAllowed "ordinary source changes may defer workflow waiting during iteration"
Assert-Selector ($sourcePlan.workflowWaitPolicy.recommendedMode -eq "opportunistic-during-iteration-final-before-handoff") "ordinary source changes must recommend opportunistic iteration and final wait"

$archivePlan = Get-SuperZipVerificationPlan -ChangedPath @("src/zip/zip_adapter.cpp")
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "security-scan") "archive parser changes must run security scan"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "short-fuzz-smoke") "archive parser changes must run short fuzz smoke"
Assert-Selector (Test-Workflow -Plan $archivePlan -Name "fuzzing") "archive parser changes must wait for fuzzing"

$guiPlan = Get-SuperZipVerificationPlan -ChangedPath @("src/app/main_window.cpp")
Assert-Selector (Test-RequiredCommand -Plan $guiPlan -Id "gui-smoke") "GUI changes must run GUI smoke"
Assert-Selector (Test-Workflow -Plan $guiPlan -Name "windows-ci") "GUI changes must wait for windows-ci"

$workflowPlan = Get-SuperZipVerificationPlan -ChangedPath @(".github/workflows/security-code-scanning.yml")
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "security-scan") "workflow changes must run security scan"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "security") "workflow changes must wait for security"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "scorecard") "workflow changes must wait for scorecard"
Assert-Selector $workflowPlan.postPushAuditRequired "workflow changes must require post-push audit"

$mcpPlan = Get-SuperZipVerificationPlan -ChangedPath @("mcp/superzip_mcp.py")
Assert-Selector $mcpPlan.scope.fullEscalationRequired "MCP verifier-adjacent changes must escalate"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "mcp-python-compile") "MCP changes must compile Python"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "verification-selector-self-test") "MCP/verifier changes must self-test selector"

$verifierPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/superzip_verification.psm1")
Assert-Selector $verifierPlan.scope.fullEscalationRequired "verification tool changes must escalate"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "verification-selector-self-test") "verification tool changes must self-test selector"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "package-smoke") "full escalation must include package smoke"
Assert-Selector (Test-Workflow -Plan $verifierPlan -Name "greenbone-openvas-vulnetix") "full escalation must include Greenbone/Vulnetix workflow"
Assert-Selector $verifierPlan.workflowWaitPolicy.immediateRequired "verification changes must require immediate final workflow waiting"
Assert-Selector (-not $verifierPlan.workflowWaitPolicy.deferAllowed) "verification changes must not allow deferred workflow waiting by default"

$unknownPlan = Get-SuperZipVerificationPlan -ChangedPath @("unexpected/new-area.file")
Assert-Selector $unknownPlan.scope.fullEscalationRequired "unknown paths must escalate"
Assert-Selector ($unknownPlan.scope.unknownPaths.Count -eq 1) "unknown path must be reported"

$forcedPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/targeted-verification.md") -SuspectGlobalBug
Assert-Selector $forcedPlan.scope.fullEscalationRequired "SuspectGlobalBug must escalate even for docs"
Assert-Selector (Test-RequiredCommand -Plan $forcedPlan -Id "security-scan") "forced full profile must include security scan"

Assert-Selector ((Invoke-WaiterSmoke -Arguments @("-ChangedPath", "docs/targeted-verification.md", "-Mode", "defer")) -eq 0) "waiter must handle docs-only no-workflow mode without GitHub"
Assert-Selector ((Invoke-WaiterSmoke -Arguments @("-ChangedPath", "src/core/checksum.cpp", "-Mode", "defer")) -eq 0) "waiter must allow defer for ordinary source changes"
Assert-Selector ((Invoke-WaiterSmoke -Arguments @("-ChangedPath", "tools/superzip_verification.psm1", "-Mode", "defer")) -ne 0) "waiter must reject defer for verifier changes"

Write-Host "Verification selector self-test passed."
