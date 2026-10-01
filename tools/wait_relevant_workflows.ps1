param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "HEAD~1",
    [string]$HeadRef = "HEAD",
    [string]$Repository = "",
    [string]$Commit = "",
    [string[]]$WorkflowName = @(),
    [ValidateSet("final", "opportunistic", "defer")]
    [string]$Mode = "final",
    [ValidateRange(1, 1440)][int]$TimeoutMinutes = 60,
    [ValidateRange(1, 120)][int]$PollSeconds = 30,
    [ValidateRange(1, 1440)][int]$MissingWorkflowGraceMinutes = 8,
    [switch]$Full,
    [switch]$IncludeLongRunning,
    [switch]$FinalCommit,
    [switch]$AllowCriticalDefer,
    [switch]$SkipPostPushAudit
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force
. (Join-Path $PSScriptRoot 'github_repository.ps1')

if ($FinalCommit.IsPresent -and ($Mode -ne 'final' -or $SkipPostPushAudit.IsPresent)) {
    throw '-FinalCommit requires -Mode final without -SkipPostPushAudit. Final acceptance cannot be deferred.'
}

# Purpose: Resolve the commit SHA to inspect in GitHub Actions.
# Inputs: `Commit` may explicitly provide a ref or SHA; otherwise `HEAD` is used.
# Outputs: Returns a full commit SHA or throws when a short/ambiguous ref cannot be resolved locally.
function Resolve-WorkflowCommit {
    param([string]$Commit)

    $candidate = if (-not [string]::IsNullOrWhiteSpace($Commit)) {
        $Commit.Trim()
    } else {
        "HEAD"
    }
    if ($candidate -match '^[0-9a-fA-F]{40}$') { return $candidate.ToLowerInvariant() }
    $resolved = & git -C $repoRoot rev-parse --verify --quiet "$candidate^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resolved)) {
        throw "Cannot resolve commit '$candidate' to a full local commit SHA. Fetch the branch or pass a full 40-character SHA before waiting for workflows."
    }
    return (@($resolved)[0]).Trim()
}

# Purpose: Verify GitHub CLI access before workflow polling begins.
# Inputs: `Repository` is an owner/repo slug.
# Outputs: Throws on missing `gh`, missing authentication, missing authorization, or API failure.
function Test-GitHubCliReady {
    param([Parameter(Mandatory = $true)][string]$Repository)

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "GitHub CLI 'gh' is required before waiting for remote workflows."
    }

    $result = & gh api -H "Accept: application/vnd.github+json" "/repos/$Repository" --jq ".full_name" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $detail = (@($result) | Select-Object -First 3) -join " "
        throw "GitHub CLI preflight failed for $Repository. Set GH_TOKEN for this PowerShell process or run 'gh auth login' before waiting. gh output: $detail"
    }
}

# Purpose: Fetch GitHub workflow runs for a commit.
# Inputs: `Repository` is owner/repo and `Commit` is a full commit SHA.
# Outputs: Returns normalized run objects from the GitHub Actions API.
function Get-WorkflowRunForCommit {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Commit
    )

    $json = & gh api --paginate --slurp -H "Accept: application/vnd.github+json" "/repos/$Repository/actions/runs?head_sha=$Commit&per_page=100" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $detail = (@($json) | Select-Object -First 3) -join " "
        throw "GitHub Actions run lookup failed for $Repository@$Commit. Stop instead of polling stale/missing status. gh output: $detail"
    }
    $response = $json | ConvertFrom-Json
    $pages = @($response)
    if ($pages.Count -eq 0) { throw 'GitHub Actions returned no valid response pages.' }
    $normalizedRuns = foreach ($page in $pages) {
        if ($null -eq $page.workflow_runs) { throw 'GitHub Actions response lacks workflow_runs.' }
        foreach ($run in $page.workflow_runs) {
            if ($run.head_sha -ne $Commit -or -not $run.id -or -not $run.name -or -not $run.status) {
                throw 'GitHub Actions returned invalid or wrong-commit workflow evidence.'
            }
            [pscustomobject][ordered]@{
                databaseId = $run.id
                workflowName = $run.name
                name = $run.display_title
                status = $run.status
                conclusion = $run.conclusion
                url = $run.html_url
                updatedAt = $run.updated_at
            }
        }
    }
    return @($normalizedRuns)
}

# Purpose: Return whether all selected workflow runs have completed successfully.
# Inputs: `Runs` is the exact-commit GitHub run list and `WorkflowName` is the selected workflow-name set.
# Outputs: Uses the newest non-cancelled duplicate per workflow and returns completion status.
function Test-SelectedWorkflowCompletion {
    param(
        [object[]]$Runs,
        [string[]]$WorkflowName
    )

    $missing = @()
    $failed = @()
    $running = @()
    foreach ($name in $WorkflowName) {
        $candidates = @($Runs | Where-Object { $_.workflowName -eq $name } |
                Sort-Object -Property @{ Expression = { [long]$_.databaseId }; Descending = $true })
        if ($candidates.Count -eq 0) {
            $missing += $name
            continue
        }
        $run = @($candidates | Where-Object { $_.conclusion -ne "cancelled" } | Select-Object -First 1)
        if ($run.Count -eq 0) {
            $run = @($candidates[0])
        }
        if ($run[0].status -ne "completed") {
            $running += ("{0}:{1}" -f $name, $run[0].status)
            continue
        }
        if ($run[0].conclusion -ne "success") {
            $failed += ("{0}:{1} {2}" -f $name, $run[0].conclusion, $run[0].url)
        }
    }
    return [pscustomobject][ordered]@{
        complete = ($missing.Count -eq 0 -and $running.Count -eq 0 -and $failed.Count -eq 0)
        missing = @($missing)
        running = @($running)
        failed = @($failed)
    }
}

# Purpose: Persist the exact-SHA checkpoint without promoting pending checks or audits to passed.
# Inputs: Root is this checkout; Repository/Commit identify evidence; Status, Runs and audit state are observed;
#         Selected names the checked gates and FinalScopeCovered states whether all planned gates were selected.
# Outputs: Atomically replaces an ignored JSON snapshot; rejects linked destinations and returns its path.
function Save-WorkflowCheckpoint {
    param(
        [string]$Root, [string]$Repository,
        [ValidatePattern('^[0-9a-f]{40}$')][string]$Commit,
        [string]$Mode, [string[]]$Selected, $Status, [object[]]$Runs, [bool]$FinalScopeCovered,
        [ValidateSet('not-required', 'pending', 'passed', 'failed')][string]$AuditState
    )
    $out = Join-Path $Root 'out'
    $directory = Join-Path $out 'workflow-status'
    foreach ($item in @($out, $directory)) {
        New-Item -ItemType Directory -Path $item -Force | Out-Null
        if ((Get-Item -LiteralPath $item).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing a linked workflow checkpoint directory.'
        }
    }
    $path = Join-Path $directory "$Commit.json"
    if (Test-Path -LiteralPath $path) {
        $existing = Get-Item -LiteralPath $path
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing a linked or non-file workflow checkpoint.'
        }
    }
    $record = [ordered]@{
        schemaVersion = 1; repository = $Repository; commit = $Commit
        checkedAtUtc = [DateTime]::UtcNow.ToString('o'); mode = $Mode
        selectedWorkflows = @($Selected); status = $Status; runs = @($Runs)
        finalScopeCovered = $FinalScopeCovered
        postPushAudit = $AuditState
        accepted = ($Mode -eq 'final' -and $FinalScopeCovered -and $Status.complete -and $AuditState -in @('passed', 'not-required'))
    }
    $temporary = Join-Path $directory ([guid]::NewGuid().ToString('N') + '.partial')
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        try { $writer.Write(($record | ConvertTo-Json -Depth 6)) } finally { $writer.Dispose() }
        Move-Item -LiteralPath $temporary -Destination $path -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
    return $path
}

$checkpoint = if ($Mode -eq 'final') { 'final' } else { 'intermediate' }
$plan = Get-SuperZipVerificationPlan -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef -SuspectGlobalBug:$Full -Checkpoint $checkpoint
if ($Mode -eq 'final' -and $plan.postPushAuditRequired -and $SkipPostPushAudit.IsPresent) {
    throw 'A required post-push audit cannot be skipped in final mode.'
}
$selected = if (@($WorkflowName).Count -gt 0) { @($WorkflowName) } else { @($plan.postPushWorkflows) }
if (@($WorkflowName).Count -eq 0 -and ($IncludeLongRunning.IsPresent -or $Mode -eq 'final')) {
    $selected = @($selected) + @($plan.longRunningPostPushWorkflows)
    $selected = @($selected | Select-Object -Unique)
}
$finalScopeCovered = (@($plan.allPostPushWorkflows | Where-Object { $_ -notin $selected }).Count -eq 0)
if ($FinalCommit.IsPresent -and -not $finalScopeCovered) {
    throw '-FinalCommit selection must include every workflow required by the plan.'
}
if ($selected.Count -eq 0) {
    if (@($plan.longRunningPostPushWorkflows).Count -gt 0 -and -not ($IncludeLongRunning.IsPresent -or $FinalCommit.IsPresent)) {
        Write-Output "Only long-running post-push workflows are selected for this change: $($plan.longRunningPostPushWorkflows -join ', ')"
        Write-Output "Use -Mode opportunistic -IncludeLongRunning for a periodic status sample, or -Mode final -FinalCommit before final handoff."
    } else {
        Write-Output "No relevant post-push workflows selected for this change."
    }
    return
}
if (@($plan.longRunningPostPushWorkflows).Count -gt 0 -and -not ($IncludeLongRunning.IsPresent -or $Mode -eq 'final') -and @($WorkflowName).Count -eq 0) {
    Write-Output "Long-running workflows are not waited by default: $($plan.longRunningPostPushWorkflows -join ', ')"
    Write-Output "Use -Mode opportunistic -IncludeLongRunning to check them during iteration, or -Mode final -FinalCommit before final handoff/release."
}
if ($Mode -eq "defer") {
    Write-Output 'Mode=defer is a compatibility alias for one opportunistic status check, not an unchecked deferral.'
    $Mode = 'opportunistic'
}
if ($AllowCriticalDefer.IsPresent) {
    Write-Output 'AllowCriticalDefer is obsolete: intermediate checkpoints are automatic; final gates remain mandatory.'
}

$repositorySlug = Resolve-GitHubRepository -Repository $Repository -RepositoryRoot $repoRoot
$commitSha = Resolve-WorkflowCommit -Commit $Commit
Test-GitHubCliReady -Repository $repositorySlug
$deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
$missingDeadline = [DateTime]::UtcNow.AddMinutes($MissingWorkflowGraceMinutes)
Write-Output ("Workflow checkpoint mode={0} on {1}@{2}: {3}" -f $Mode, $repositorySlug, $commitSha, ($selected -join ', '))
$auditState = if ($plan.postPushAuditRequired) { 'pending' } else { 'not-required' }
$lastSummary = ''
$delay = $PollSeconds

while ($true) {
    $runs = Get-WorkflowRunForCommit -Repository $repositorySlug -Commit $commitSha
    $status = Test-SelectedWorkflowCompletion -Runs $runs -WorkflowName $selected
    $report = Save-WorkflowCheckpoint -Root $repoRoot -Repository $repositorySlug -Commit $commitSha `
        -Mode $Mode -Selected $selected -Status $status -Runs $runs -AuditState $auditState -FinalScopeCovered $finalScopeCovered
    if ($status.failed.Count -gt 0) {
        throw "Relevant workflow failure(s): $($status.failed -join '; ')"
    }
    if ($status.complete) {
        Write-Output "Relevant workflows completed successfully: $($selected -join ', ')"
        break
    }
    $summary = "Pending. Missing: $($status.missing -join ', ') Running: $($status.running -join ', ')"
    if ($Mode -eq 'opportunistic') {
        Write-Output $summary
        Write-Output "Snapshot: $report. Continue independent work; final acceptance and required audit remain pending."
        return
    }
    if ([DateTime]::UtcNow -ge $deadline) {
        throw "Timed out waiting for relevant workflows. Missing: $($status.missing -join ', ') Running: $($status.running -join ', ')"
    }
    if ($status.missing.Count -gt 0 -and [DateTime]::UtcNow -ge $missingDeadline) {
        throw "Missing workflows for $repositorySlug@$commitSha after $MissingWorkflowGraceMinutes minute(s): $($status.missing -join ', '). Inspect event/path filters or an authorized validation dispatch; do not poll a nonexistent run."
    }
    if ($summary -ne $lastSummary) {
        Write-Output $summary
        $delay = $PollSeconds
    } else {
        $delay = [Math]::Min(120, [Math]::Ceiling($delay * 1.5))
    }
    $lastSummary = $summary
    $remainingSeconds = [Math]::Max(1, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
    Start-Sleep -Seconds ([Math]::Min($delay, $remainingSeconds))
}

if ($Mode -eq 'final' -and $plan.postPushAuditRequired) {
    try {
        & (Join-Path $PSScriptRoot "github_post_push_audit.ps1") -Repository $repositorySlug
        $auditState = 'passed'
    } catch {
        Save-WorkflowCheckpoint -Root $repoRoot -Repository $repositorySlug -Commit $commitSha `
            -Mode $Mode -Selected $selected -Status $status -Runs $runs -AuditState 'failed' -FinalScopeCovered $finalScopeCovered | Out-Null
        throw
    }
}
Write-Output ("Snapshot: " + (Save-WorkflowCheckpoint -Root $repoRoot -Repository $repositorySlug -Commit $commitSha `
        -Mode $Mode -Selected $selected -Status $status -Runs $runs -AuditState $auditState -FinalScopeCovered $finalScopeCovered))
if ($Mode -eq 'opportunistic') { Write-Output 'Final acceptance remains pending even when this intermediate workflow sample is green.' }
