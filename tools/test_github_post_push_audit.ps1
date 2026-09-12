$ErrorActionPreference = 'Stop'
$auditReplies = [System.Collections.Generic.Queue[object]]::new()

# Purpose: Replace GitHub CLI calls with ordered offline responses for the production audit.
# Inputs: The implicit command arguments must request the next fixture endpoint.
# Outputs: Emits the fixture response and sets the same process-status variable used by a native CLI.
function Invoke-TestGitHub {
    if ($auditReplies.Count -eq 0) {
        throw 'Unexpected GitHub CLI call during offline audit testing.'
    }
    $reply = $auditReplies.Dequeue()
    if ($args.Count -lt 2 -or $args[0] -ne 'api' -or $args[1] -notlike $reply.Endpoint) {
        throw 'The audit called an unexpected API endpoint.'
    }
    $global:LASTEXITCODE = $reply.ExitCode
    return $reply.Body
}
Set-Alias -Name gh -Value Invoke-TestGitHub -Scope Script

# Purpose: Describe one offline GitHub CLI result without making a network request.
# Inputs: Endpoint wildcard, native exit status, and response body.
# Outputs: Returns a queue entry consumed by Invoke-TestGitHub.
function Get-ApiReply {
    param([string]$Endpoint, [int]$ExitCode = 0, [AllowNull()][string]$Body)
    return [pscustomobject]@{ Endpoint = $Endpoint; ExitCode = $ExitCode; Body = $Body }
}

# Purpose: Run the actual audit with bounded mock responses and assert its pass/fail decision.
# Inputs: Name labels the scenario; Responses fixes all API output; Failure contains required diagnostic text.
# Outputs: Throws on false success, unexpected failure, or unconsumed API responses.
function Test-AuditCase {
    param([string]$Name, [object[]]$Responses, [string]$Failure = '')
    $auditReplies.Clear()
    foreach ($response in $Responses) { $auditReplies.Enqueue($response) }
    $message = ''
    try {
        & (Join-Path $PSScriptRoot 'github_post_push_audit.ps1') -Repository 'fixture/repository' 6>&1 | Out-Null
    } catch {
        $message = $_.Exception.Message
    }
    if (($Failure -eq '' -and $message -ne '') -or ($Failure -ne '' -and -not $message.Contains($Failure))) {
        throw "Post-push audit regression: $Name returned an unexpected decision: $message"
    }
    if ($auditReplies.Count -ne 0) {
        throw "Post-push audit regression: $Name left expected API calls untested."
    }
    Write-Output "Post-push audit scenario passed: $Name"
}

$deployments = 'repos/fixture/repository/deployments'
$alerts = 'repos/fixture/repository/code-scanning/alerts*'
$emptyDeployments = Get-ApiReply $deployments 0 '0'
$emptyAlerts = Get-ApiReply $alerts 0 '[]'
$approvedAlert = [pscustomobject]@{ number = 1; tool = @{ name = 'Scorecard' }; rule = @{ id = 'CodeReviewID' } }
$approvedJson = ConvertTo-Json -InputObject @($approvedAlert) -Depth 5 -Compress
$unapprovedJson = '[{"number":2,"tool":{"name":"CodeQL"},"rule":{"id":"cpp/test-rule"}}]'

Test-AuditCase 'empty successful snapshot' @($emptyDeployments, $emptyAlerts)
Test-AuditCase 'existing approved residual' @($emptyDeployments, (Get-ApiReply $alerts 0 $approvedJson))
Test-AuditCase 'unapproved finding' @($emptyDeployments, (Get-ApiReply $alerts 0 $unapprovedJson)) 'Unapproved code-scanning'
Test-AuditCase 'existing deployment' @((Get-ApiReply $deployments 0 '1')) 'deployments are forbidden'
Test-AuditCase 'failed deployments with zero body' @((Get-ApiReply $deployments 1 '0')) 'deployments API failed'
Test-AuditCase 'failed alerts with empty array body' @($emptyDeployments, (Get-ApiReply $alerts 1 '[]')) 'code-scanning API failed'
Test-AuditCase 'failed deployments without body' @((Get-ApiReply $deployments 1 '')) 'deployments API failed'
Test-AuditCase 'failed alerts without body' @($emptyDeployments, (Get-ApiReply $alerts 1 '')) 'code-scanning API failed'
foreach ($invalid in @('', 'invalid', '-1', '999999999999999999999')) {
    Test-AuditCase 'invalid deployments count' @((Get-ApiReply $deployments 0 $invalid)) 'invalid deployment count'
}
Test-AuditCase 'empty alerts response' @($emptyDeployments, (Get-ApiReply $alerts 0 '')) 'empty code-scanning response'
Test-AuditCase 'invalid alerts JSON' @($emptyDeployments, (Get-ApiReply $alerts 0 '{')) 'invalid code-scanning JSON'
foreach ($invalid in @('null', '{}', '0')) {
    Test-AuditCase 'invalid alerts root' @($emptyDeployments, (Get-ApiReply $alerts 0 $invalid)) 'non-array code-scanning response'
}
foreach ($invalid in @('[null]', '[[]]', '[{}]')) {
    Test-AuditCase 'invalid alert record' @($emptyDeployments, (Get-ApiReply $alerts 0 $invalid)) 'invalid code-scanning record'
}
$fullPage = ConvertTo-Json -InputObject @((1..100) | ForEach-Object { $approvedAlert }) -Depth 5 -Compress
Test-AuditCase 'complete pagination' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), $emptyAlerts)
Test-AuditCase 'unapproved finding on second page' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), (Get-ApiReply $alerts 0 $unapprovedJson)) 'Unapproved code-scanning'
Test-AuditCase 'failed second page' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), (Get-ApiReply $alerts 1 '[]')) 'code-scanning API failed'
$global:LASTEXITCODE = 0
Write-Output 'Post-push audit offline regression tests passed (23 scenarios).'
