$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$waiter = Join-Path $PSScriptRoot 'wait_relevant_workflows.ps1'
$testCommit = [guid]::NewGuid().ToString('N') + 'abcdef12'
$report = Join-Path $root "out/workflow-status/$testCommit.json"
$state = @{
    Commit = $testCommit; LookupCount = 0; ApiFailure = $false
    Malformed = $false; WrongCommit = $false; Conclusion = ''
    Missing = $false; CompleteAfterLookup = 0; AllowSleep = $false
    Sleeps = [Collections.Generic.List[int]]::new()
}

# Purpose: Emulate GitHub API responses without network access or authentication.
# Inputs: Arguments are the waiter's fixed gh API invocation; closure state selects the scenario.
# Outputs: Emits paginated exact-commit JSON or a failure and sets the native exit status.
$githubStub = {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $global:LASTEXITCODE = 0
    if ($Arguments -contains '.full_name') { return 'example/SuperZip' }
    ++$state.LookupCount
    if ($state.ApiFailure) { $global:LASTEXITCODE = 1; return 'test: API unavailable' }
    if ($state.Malformed) { return '[{}]' }
    if ($state.Missing) { return '[{"workflow_runs":[]}]' }
    $sha = if ($state.WrongCommit) { '0' * 40 } else { $state.Commit }
    $conclusion = if ($state.CompleteAfterLookup -gt 0 -and $state.LookupCount -ge $state.CompleteAfterLookup) {
        'success'
    } else { $state.Conclusion }
    $run = [ordered]@{
        id = 1; name = 'lint'; head_sha = $sha; display_title = 'test checkpoint'
        status = if ($conclusion) { 'completed' } else { 'in_progress' }
        conclusion = $conclusion; html_url = 'https://example.test/run/1'; updated_at = '2026-10-01T00:00:00Z'
    }
    return ConvertTo-Json -InputObject @(@{ workflow_runs = @() }, @{ workflow_runs = @($run) }) -Depth 5
}.GetNewClosure()
Set-Item -Path function:gh -Value $githubStub

# Purpose: Observe final polling intervals and reject sleeps in intermediate tests.
# Inputs: Seconds is the requested interval; closure state controls simulated final waiting.
# Outputs: Records the interval without sleeping, or throws for an intermediate checkpoint.
$sleepStub = {
    param([int]$Seconds)
    if ($state.AllowSleep) { $state.Sleeps.Add($Seconds); return }
    throw "Unexpected checkpoint sleep: $Seconds"
}.GetNewClosure()
Set-Item -Path function:Start-Sleep -Value $sleepStub

# Purpose: Assert the checkpoint contract with a readable diagnostic.
# Inputs: Condition is the expected invariant and Message describes a violation.
# Outputs: Throws on a failed invariant.
function Assert-Checkpoint {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Workflow checkpoint assertion: $Message" }
}

# Purpose: Exercise one expected failing checkpoint without suppressing its diagnosis.
# Inputs: Expected is the exception text pattern; WaitArguments selects the scenario.
# Outputs: Throws if the operation unexpectedly succeeds or fails for a different reason.
function Assert-CheckpointFailure {
    param([string]$Expected, [hashtable]$WaitArguments)
    $caught = $false
    try { & $waiter @WaitArguments | Out-Null } catch {
        $caught = $true
        Assert-Checkpoint ($_.Exception.Message -like $Expected) "unexpected error: $($_.Exception.Message)"
    }
    Assert-Checkpoint $caught 'expected a failing checkpoint'
}

$arguments = @{
    ChangedPath = @('tools/superzip_verification.psm1'); Repository = 'example/SuperZip'
    Commit = $testCommit; WorkflowName = @('lint'); Mode = 'opportunistic'
}
try {
    & $waiter @arguments | Out-Null
    $pending = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint ($state.LookupCount -eq 1) 'intermediate verifier changes must fetch status exactly once'
    Assert-Checkpoint (-not $pending.accepted -and -not $pending.status.complete -and $pending.postPushAudit -eq 'pending') `
        'running checks and the audit must remain pending'
    Assert-Checkpoint ($pending.commit -eq $testCommit -and $pending.status.running[0] -eq 'lint:in_progress') `
        'snapshot must preserve exact-SHA running evidence'
    $state.Missing = $true
    & $waiter @arguments | Out-Null
    $missing = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint ($missing.status.missing[0] -eq 'lint' -and -not $missing.accepted) 'missing checks must remain pending, not pass'
    $state.Missing = $false
    $state.Conclusion = 'success'
    & $waiter @arguments | Out-Null
    $green = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint ($green.status.complete -and -not $green.accepted -and $green.postPushAudit -eq 'pending') `
        'green intermediate runs must not become final acceptance or trigger an audit'
    $arguments.Mode = 'defer'
    $before = $state.LookupCount
    & $waiter @arguments | Out-Null
    Assert-Checkpoint ($state.LookupCount -eq $before + 1) 'legacy defer must sample the API rather than hide failures'
    $state.Conclusion = 'failure'
    Assert-CheckpointFailure '*Relevant workflow failure*' $arguments
    $failed = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint ($failed.status.failed.Count -eq 1 -and -not $failed.accepted) 'failure evidence must be retained'
    $state.ApiFailure = $true
    Assert-CheckpointFailure '*GitHub Actions run lookup failed*test: API unavailable*' $arguments
    $state.ApiFailure = $false
    $state.Malformed = $true
    Assert-CheckpointFailure '*response lacks workflow_runs*' $arguments
    $state.Malformed = $false
    $state.WrongCommit = $true
    Assert-CheckpointFailure '*wrong-commit workflow evidence*' $arguments
    $state.WrongCommit = $false
    $state.Conclusion = 'success'
    $arguments.Mode = 'final'
    $arguments.FinalCommit = $true
    Assert-CheckpointFailure '*selection must include every workflow*' $arguments
    $arguments.Remove('FinalCommit')
    $arguments.SkipPostPushAudit = $true
    Assert-CheckpointFailure '*audit cannot be skipped*' $arguments
    $arguments.Remove('SkipPostPushAudit')
    $arguments.ChangedPath = @('README.md')
    & $waiter @arguments | Out-Null
    $accepted = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint ($accepted.accepted -and $accepted.postPushAudit -eq 'not-required') 'final successful selected checks must be accepted'
    $arguments.ChangedPath = @('src/core/checksum.cpp')
    & $waiter @arguments | Out-Null
    $narrow = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    Assert-Checkpoint (-not $narrow.accepted -and -not $narrow.finalScopeCovered) 'a narrower selected subset is not final acceptance'
    $arguments.ChangedPath = @('README.md')
    $state.Conclusion = ''
    $state.AllowSleep = $true
    $state.CompleteAfterLookup = $state.LookupCount + 6
    & $waiter @arguments | Out-Null
    Assert-Checkpoint (($state.Sleeps -join ',') -eq '30,45,68,102,120') 'unchanged final checks must back off up to 120 seconds'
} finally {
    if (Test-Path -LiteralPath $report -PathType Leaf) { Remove-Item -LiteralPath $report }
    Remove-Item -Path function:gh, function:Start-Sleep
}
$global:LASTEXITCODE = 0
Write-Output 'Workflow checkpoint tests passed (offline, no timed workloads).'
