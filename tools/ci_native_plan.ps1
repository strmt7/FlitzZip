# Purpose: Select or execute hosted native work from explicit paths or the complete Git event range.
# Inputs: ChangedPath or BaseRef/HeadRef, explicit Full, optional RunTests and GitHub output destination.
# Outputs: Emits portable decisions and known boolean outputs, or executes the selected CPU validation command.
param(
    [string[]]$ChangedPath = @(), [string]$BaseRef = '', [string]$HeadRef = 'HEAD',
    [switch]$Full, [switch]$RunTests, [string]$GitHubOutput = '',
    [ValidateSet('push', 'pull_request', 'workflow_dispatch')][string]$EventName,
    [string]$PushBase = '', [string]$PullRequestBase = ''
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'superzip_verification.psm1') -Force
if ($EventName) {
    if ($ChangedPath.Count -or $BaseRef) { throw 'Select an explicit change range or a Git event, not both.' }
    $nativeEvent = Get-SuperZipNativeCiEvent -EventName $EventName -PushBase $PushBase -PullRequestBase $PullRequestBase
    $paths = $nativeEvent.paths
    $Full = $Full.IsPresent -or $nativeEvent.full
} else { $paths = @(Get-SuperZipChangedPath -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef) }
$plan = Get-SuperZipHostedNativePlan -Paths $paths -Full:$Full
if ($RunTests.IsPresent) {
    if ($plan.testMode -ne 'none') {
        $receipt = & py -3 (Join-Path $PSScriptRoot 'native_build_receipt.py') validate --configuration Release
        if ($LASTEXITCODE -ne 0) { throw 'Hosted native tests require a current successful build receipt.' }
        Assert-SuperZipCpuValidationReceipt -Receipt (($receipt -join [Environment]::NewLine) | ConvertFrom-Json)
    }
    Invoke-SuperZipHostedNativeTest -Plan $plan
    return
}
$summary = [ordered]@{ native_build = $plan.build; native_tests = $plan.testMode -ne 'none'
    native_test_mode = $plan.testMode; format_matrix = $plan.formatMatrix; policy_scan = $plan.securityPolicy; gpu_execution_qualified = $false }
if ($GitHubOutput) {
    foreach ($name in @('native_build', 'native_tests', 'format_matrix', 'policy_scan')) {
        [IO.File]::AppendAllText($GitHubOutput, "$name=$($summary[$name].ToString().ToLowerInvariant())`n")
    }
}
ConvertTo-Json -InputObject $summary -Compress
