$ErrorActionPreference = 'Stop'
$auditReplies = [System.Collections.Generic.Queue[object]]::new()
$auditScenarioCount = 0
$auditGitRemote = 'https://github.com/fixture/repository.git'
$auditGitExitCode = 0

# Purpose: Exercise repository resolution without reading or altering real Git configuration.
# Inputs: Only the production remote.origin.url query is accepted; scoped fixture values supply its result.
# Outputs: Returns mock remote text and status or rejects an unexpected Git command.
function Invoke-TestGitRemote {
    if ($args.Count -ne 5 -or $args[0] -ne '-C' -or
        $args[1] -ne (Split-Path -Parent $PSScriptRoot) -or
        ($args[2..4] -join ' ') -ne 'config --get remote.origin.url') {
        throw 'Unexpected Git command during audit tests.'
    }
    $global:LASTEXITCODE = $auditGitExitCode
    return $auditGitRemote
}
Set-Alias -Name git -Value Invoke-TestGitRemote -Scope Script

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
# Inputs: Scenario label, API fixtures, expected error, optional all-state selection and new report destination.
# Outputs: Throws on false success, unexpected failure, or unconsumed API responses.
function Test-AuditCase {
    param(
        [string]$Name, [object[]]$Responses, [string]$Failure = '',
        [switch]$IncludeHistory, [string]$HistoryReportPath = '',
        [AllowEmptyString()][string]$Repository = 'fixture/repository', [string]$Commit = ''
    )
    $auditReplies.Clear()
    foreach ($response in $Responses) { $auditReplies.Enqueue($response) }
    $message = ''
    try {
        & (Join-Path $PSScriptRoot 'github_post_push_audit.ps1') -Repository $Repository -Commit $Commit `
            -IncludeHistory:$IncludeHistory -HistoryReportPath $HistoryReportPath 6>&1 | Out-Null
    } catch {
        $message = $_.Exception.Message
    }
    $script:lastAuditMessage = $message
    if (($Failure -eq '' -and $message -ne '') -or ($Failure -ne '' -and -not $message.Contains($Failure))) {
        throw "Post-push audit regression: $Name returned an unexpected decision: $message"
    }
    if ($auditReplies.Count -ne 0) {
        throw "Post-push audit regression: $Name left expected API calls untested."
    }
    $script:auditScenarioCount += 1
    Write-Output "Post-push audit scenario passed: $Name"
}

$deployments = 'repos/fixture/repository/deployments'
$alerts = 'repos/fixture/repository/code-scanning/alerts*'
$emptyDeployments = Get-ApiReply $deployments 0 '0'
$emptyAlerts = Get-ApiReply $alerts 0 '[]'
$approvedAlert = [pscustomobject]@{ number = 1; state = 'open'; tool = @{ name = 'Scorecard' }; rule = @{ id = 'CodeReviewID' } }
$approvedJson = ConvertTo-Json -InputObject @($approvedAlert) -Depth 5 -Compress
$unapprovedJson = '[{"number":2,"state":"open","tool":{"name":"CodeQL"},"rule":{"id":"cpp/test-rule"}}]'

# Both entry points must retain the same checkout-bound, non-disclosing resolver.
foreach ($entryPoint in @('github_post_push_audit.ps1', 'wait_relevant_workflows.ps1')) {
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot $entryPoint), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) { throw 'GitHub entry point failed to parse.' }
    $definitions = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Resolve-GitHubRepository'
    }, $true))
    $calls = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Resolve-GitHubRepository'
    }, $true))
    $imports = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.CommandAst] -and
            $node.InvocationOperator -eq [System.Management.Automation.Language.TokenKind]::Dot -and
            $node.Extent.Text.Contains("'github_repository.ps1'")
    }, $true))
    if ($definitions.Count -ne 0 -or $imports.Count -ne 1 -or $calls.Count -ne 1 -or
        $calls[0].Extent.Text -notmatch '-RepositoryRoot\s+\$repoRoot') {
        throw "GitHub entry point bypassed the shared checkout-bound resolver: $entryPoint"
    }
    $script:auditScenarioCount += 1
}

Test-AuditCase 'empty successful snapshot' @($emptyDeployments, $emptyAlerts)
foreach ($remote in @('https://github.com/fixture/repository.git', 'https://github.com/fixture/repository',
        'git@github.com:fixture/repository.git', 'ssh://git@github.com/fixture/repository.git')) {
    $auditGitRemote = $remote
    Test-AuditCase 'credential-free remote resolves expected repository' @($emptyDeployments, $emptyAlerts) -Repository ''
}
foreach ($remote in @('https://examplegithub.com/fixture/repository.git', 'https://github.com.bad.invalid/fixture/repository.git',
        ('https://' + 'fixture-user:fixture-value' + '@github.com/fixture/repository.git'))) {
    $auditGitRemote = $remote
    Test-AuditCase 'untrusted or credential-bearing remote rejected' @() 'Cannot parse GitHub repository' -Repository ''
    if ($lastAuditMessage.Contains($remote)) { throw 'Rejected remote value leaked into audit diagnostics.' }
}
$auditGitRemote = 'https://github.com/fixture/repository.git'
$auditGitExitCode = 1
Test-AuditCase 'failed remote query cannot use plausible output' @() 'reading remote.origin.url failed' -Repository ''
$auditGitExitCode = 0
$auditGitRemote = ''
Test-AuditCase 'empty remote query rejected' @() 'remote.origin.url is unset' -Repository ''
Test-AuditCase 'existing approved residual' @($emptyDeployments, (Get-ApiReply $alerts 0 $approvedJson))
Test-AuditCase 'unapproved finding' @($emptyDeployments, (Get-ApiReply $alerts 0 $unapprovedJson)) 'Unapproved code-scanning'
$priorityJson = '[{"number":100,"state":"open","tool":{"name":"CodeQL"},"rule":{"id":"cpp/quality"}},' +
    '{"number":2,"state":"open","tool":{"name":"CodeQL"},"rule":{"id":"cpp/critical","security_severity_level":"critical"}},' +
    '{"number":3,"state":"open","tool":{"name":"CodeQL"},"rule":{"id":"cpp/high","security_severity_level":"high"}}]'
Test-AuditCase 'critical examples precede newer quality alerts' @($emptyDeployments, (Get-ApiReply $alerts 0 $priorityJson)) ("First 12 examples (at most):`n2 CodeQL/cpp/critical no file`n3 CodeQL/cpp/high no file`n100 CodeQL/cpp/quality no file")
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
$fullPage = ConvertTo-Json -InputObject @((1..100) | ForEach-Object {
    [pscustomobject]@{ number = $_; state = 'open'; tool = @{ name = 'Scorecard' }; rule = @{ id = 'CodeReviewID' } }
}) -Depth 5 -Compress
$secondPageFinding = $unapprovedJson.Replace('"number":2', '"number":101')
Test-AuditCase 'complete pagination' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), $emptyAlerts)
Test-AuditCase 'unapproved finding on second page' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), (Get-ApiReply $alerts 0 $secondPageFinding)) 'Unapproved code-scanning'
Test-AuditCase 'failed second page' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), (Get-ApiReply $alerts 1 '[]')) 'code-scanning API failed'
Test-AuditCase 'repeated page rejects incomplete evidence' @($emptyDeployments, (Get-ApiReply $alerts 0 $fullPage), (Get-ApiReply $alerts 0 $fullPage)) 'duplicate alert'
Test-AuditCase 'duplicate within a page rejected' @($emptyDeployments, (Get-ApiReply $alerts 0 ('[' + ($approvedJson.TrimStart('[').TrimEnd(']')) + ',' + ($approvedJson.TrimStart('[').TrimEnd(']')) + ']'))) 'duplicate alert'
Test-AuditCase 'missing incident state rejected' @($emptyDeployments, (Get-ApiReply $alerts 0 ($unapprovedJson.Replace('"state":"open",', '')))) 'invalid code-scanning record'
foreach ($state in @('fixed', 'dismissed')) {
    $closedJson = $unapprovedJson.Replace('"open"', ('"' + $state + '"'))
    Test-AuditCase 'closed result in open-only query' @($emptyDeployments, (Get-ApiReply $alerts 0 $closedJson)) 'non-open alert'
}
foreach ($state in @('closed', 'unknown', '')) {
    $invalidJson = $unapprovedJson.Replace('"open"', ('"' + $state + '"'))
    Test-AuditCase 'invalid incident state' @($emptyDeployments, (Get-ApiReply $alerts 0 $invalidJson)) 'invalid code-scanning record'
}
Test-AuditCase 'non-string state rejected' @($emptyDeployments, (Get-ApiReply $alerts 0 ($unapprovedJson.Replace('"state":"open"', '"state":["open"]')))) 'invalid code-scanning record'
$historyEndpoint = 'repos/fixture/repository/code-scanning/alerts?per_page=*'
$historyJson = '[{"number":2,"state":"fixed","tool":{"name":"CodeQL"},"rule":{"id":"cpp/test-rule"}},' +
    '{"number":3,"state":"dismissed","tool":{"name":"CodeQL"},"rule":{"id":"cpp/test-rule"},"dismissed_reason":"false positive",' +
    '"dismissed_comment":"private reviewer text must not be exported","most_recent_instance":{"location":{"path":"src/sample.cpp","start_line":12},"commit_sha":"fixture-commit","category":"c-cpp"}}]'
Test-AuditCase 'all-state history preserves closed states' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $historyJson)) -IncludeHistory
Test-AuditCase 'empty complete history' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 '[]')) -IncludeHistory
Test-AuditCase 'unapproved history entry still blocks' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $unapprovedJson)) 'Unapproved code-scanning' -IncludeHistory
Test-AuditCase 'all-state later page retained' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $fullPage), (Get-ApiReply $historyEndpoint 0 ($historyJson.Replace('"number":2', '"number":101').Replace('"number":3', '"number":102')))) -IncludeHistory
Test-AuditCase 'all-state partial history fails' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $fullPage), (Get-ApiReply $historyEndpoint 1 '[]')) 'code-scanning API failed' -IncludeHistory
Test-AuditCase 'missing history authorization for report' @() 'requires IncludeHistory' -HistoryReportPath 'unused.json'

# Purpose: Exercise the PowerShell consumer of exact source admission without invoking external tools.
# Inputs: Production Python arguments, complete JSON snapshot, and fixture output/status.
# Outputs: Verifies checkout/commit/raw-state binding and returns the configured admission response.
function Invoke-TestSourceReview {
    if ($args.Count -ne 7 -or ($args[0..3] -join ' ') -ne '-3 -m tools.scanner_metadata_review --hosted-alerts' -or
        $args[5] -ne '--commit' -or $args[6] -ne $sourceCommit -or
        (Get-Location).Path -ne (Split-Path -Parent $PSScriptRoot)) {
        throw 'Hosted source admission lost checkout or exact-commit binding.'
    }
    $snapshot = Get-Content -LiteralPath $args[4] -Raw | ConvertFrom-Json
    if (@($snapshot).Count -ne $sourceSnapshotCount -or $snapshot[0].number -ne 77 -or $snapshot[0].state -ne 'open') {
        throw 'Hosted source admission lost the complete raw alert snapshot.'
    }
    $global:LASTEXITCODE = $sourceExitCode
    return $sourceReply
}
$sourceCommit = 'a' * 40
$sourceExitCode = 0
$sourceSnapshotCount = 1
$sourceReply = '[77]'
$sourceJson = '[{"number":77,"state":"open","tool":{"name":"devskim"},"rule":{"id":"DS121708"}}]'
Set-Alias -Name py -Value Invoke-TestSourceReview -Scope Script
try {
    Test-AuditCase 'exact approved source is admitted without dismissal' @($emptyDeployments, (Get-ApiReply $alerts 0 $sourceJson)) -Commit $sourceCommit
    $sourceSnapshotCount = 2
    $mixedJson = $sourceJson.TrimEnd(']') + ',' + $unapprovedJson.TrimStart('[')
    Test-AuditCase 'source approval cannot admit another producer' @($emptyDeployments, (Get-ApiReply $alerts 0 $mixedJson)) 'Unapproved code-scanning alerts are open: 1.' -Commit $sourceCommit
    $sourceSnapshotCount = 1
    foreach ($invalidReply in @('[2]', '["77"]', '77', '{}', 'null', '[77,77]')) {
        $sourceReply = $invalidReply
        Test-AuditCase 'malformed or unrelated admission remains blocking' @($emptyDeployments, (Get-ApiReply $alerts 0 $sourceJson)) 'Hosted source review returned' -Commit $sourceCommit
    }
    $sourceReply = '[77]'
    $sourceExitCode = 1
    Test-AuditCase 'failed admission cannot consume plausible output' @($emptyDeployments, (Get-ApiReply $alerts 0 $sourceJson)) 'Exact hosted source admission failed' -Commit $sourceCommit
} finally {
    Remove-Item -LiteralPath Alias:py
}

$reportRoot = Join-Path ([IO.Path]::GetTempPath()) ('superzip-history-audit-' + [Guid]::NewGuid().ToString('N'))
$reportPath = Join-Path $reportRoot 'history.json'
try {
    Test-AuditCase 'history report export' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $historyJson)) -IncludeHistory -HistoryReportPath $reportPath
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.schema_version -ne 1 -or $report.total -ne 2 -or $report.open -ne 0 -or
        $report.fixed -ne 1 -or $report.dismissed -ne 1 -or $report.rules.Count -ne 1 -or
        $report.incidents.Count -ne 2 -or $report.incidents[1].analysis_commit -ne 'fixture-commit' -or
        $report.incidents[1].path -ne 'src/sample.cpp' -or $report.incidents[1].line -ne 12 -or
        (Get-Content -LiteralPath $reportPath -Raw).Contains('private reviewer text')) {
        throw 'History report lost evidence, conflated states, or exported private reviewer text.'
    }
    $bytesBefore = [IO.File]::ReadAllBytes($reportPath)
    Test-AuditCase 'existing report cannot be overwritten' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $historyJson)) 'already exists' -IncludeHistory -HistoryReportPath $reportPath
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($reportPath)) -ne [Convert]::ToBase64String($bytesBefore)) {
        throw 'An existing history report was changed.'
    }
    $blockedPath = Join-Path $reportRoot 'blocked.json'
    Test-AuditCase 'blocked audit retains history evidence' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $unapprovedJson)) 'Unapproved code-scanning' -IncludeHistory -HistoryReportPath $blockedPath
    if (-not (Test-Path -LiteralPath $blockedPath -PathType Leaf)) { throw 'Blocked history evidence was lost.' }
    Test-AuditCase 'non-JSON report rejected' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $historyJson)) '.json extension' -IncludeHistory -HistoryReportPath (Join-Path $reportRoot 'invalid.txt')
    Push-Location $reportRoot
    try {
        $auditGitRemote = 'https://github.com/fixture/repository.git'
        Test-AuditCase 'auto repository stays checkout-bound from another folder' @($emptyDeployments, $emptyAlerts) -Repository ''
        Test-AuditCase 'relative report follows PowerShell location' @($emptyDeployments, (Get-ApiReply $historyEndpoint 0 $historyJson)) -IncludeHistory -HistoryReportPath 'nested-relative/history.json'
        if (-not (Test-Path -LiteralPath (Join-Path $reportRoot 'nested-relative/history.json') -PathType Leaf)) {
            throw 'Relative history report was not created under the caller PowerShell location.'
        }
    } finally {
        Pop-Location
    }
} finally {
    # This UUID path was created only by these fixtures; never remove caller-supplied paths.
    $resolvedRoot = [IO.Path]::GetFullPath($reportRoot)
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $temporaryParent) {
        throw 'History fixture cleanup escaped its temporary parent.'
    }
    if (Test-Path -LiteralPath $resolvedRoot) { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
}
$global:LASTEXITCODE = 0
Write-Output "Post-push audit offline regression tests passed ($auditScenarioCount scenarios)."
