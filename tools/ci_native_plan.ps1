# Purpose: Select or execute hosted native work from explicit paths or the complete Git event range.
# Inputs: Changed paths or Git event, configuration, explicit Full, optional RunTests and output destination.
# Outputs: Emits portable decisions and known boolean outputs, or executes the selected CPU validation command.
param(
    [string[]]$ChangedPath = @(), [string]$BaseRef = '', [string]$HeadRef = 'HEAD',
    [switch]$Full, [switch]$RunTests, [string]$GitHubOutput = '',
    [ValidateSet('push', 'pull_request', 'workflow_dispatch')][string]$EventName,
    [string]$PushBase = '', [string]$PullRequestBase = '', [string]$DispatchBase = '',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'flitzzip_verification.psm1') -Force
if ($DispatchBase -and -not $EventName) { throw 'A manual comparison base requires an explicit workflow_dispatch event.' }
if ($EventName) {
    if ($ChangedPath.Count -or $BaseRef) { throw 'Select an explicit change range or a Git event, not both.' }
    $nativeEvent = Get-FlitzZipNativeCiEvent -EventName $EventName -PushBase $PushBase -PullRequestBase $PullRequestBase -DispatchBase $DispatchBase
    $paths = $nativeEvent.paths
    $Full = $Full.IsPresent -or $nativeEvent.full
} else { $paths = @(Get-FlitzZipChangedPath -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef) }
$plan = Get-FlitzZipHostedNativePlan -Paths $paths -Full:$Full -Configuration $Configuration
if ($RunTests.IsPresent) {
    if ($plan.testMode -ne 'none') {
        $receipt = & py -3 (Join-Path $PSScriptRoot 'native_build_receipt.py') validate --configuration $Configuration
        if ($LASTEXITCODE -ne 0) { throw 'Hosted native tests require a current successful build receipt.' }
        Assert-FlitzZipCpuValidationReceipt -Receipt (($receipt -join [Environment]::NewLine) | ConvertFrom-Json)
    }
    Invoke-FlitzZipHostedNativeTest -Plan $plan
    return
}
$summary = [ordered]@{ native_build = $plan.build; native_tests = $plan.testMode -ne 'none'
    native_coverage = $plan.coverage
    native_test_mode = $plan.testMode; format_matrix = $plan.formatMatrix; policy_scan = $plan.securityPolicy; gpu_execution_qualified = $false }
if ($GitHubOutput) {
    foreach ($name in @('native_build', 'native_tests', 'native_coverage', 'format_matrix', 'policy_scan')) {
        [IO.File]::AppendAllText($GitHubOutput, "$name=$($summary[$name].ToString().ToLowerInvariant())`n")
    }
}
ConvertTo-Json -InputObject $summary -Compress
