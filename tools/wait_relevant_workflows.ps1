param(
    [string[]]$ChangedPath = @(),
    [string]$BaseRef = "HEAD~1",
    [string]$HeadRef = "HEAD",
    [string]$Repository = "",
    [string]$Commit = "",
    [string[]]$WorkflowName = @(),
    [ValidateSet("final", "opportunistic", "defer")]
    [string]$Mode = "final",
    [int]$TimeoutMinutes = 60,
    [int]$PollSeconds = 30,
    [switch]$Full,
    [switch]$IncludeLongRunning,
    [switch]$FinalCommit,
    [switch]$AllowCriticalDefer,
    [switch]$SkipPostPushAudit
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

if ($FinalCommit.IsPresent -and $Mode -eq "defer") {
    throw "-FinalCommit cannot be combined with -Mode defer. Final handoff must wait for selected workflows."
}

# Purpose: Resolve the GitHub owner/repository slug for the current checkout.
# Inputs: `Repository` may explicitly provide owner/repo; otherwise `remote.origin.url` is parsed.
# Outputs: Returns owner/repo or throws when the remote is not a GitHub repository.
function Resolve-GitHubRepository {
    param([string]$Repository)

    if (-not [string]::IsNullOrWhiteSpace($Repository)) {
        if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
            throw "Repository must use owner/repo form."
        }
        return $Repository
    }
    $remote = (git -C $repoRoot config --get remote.origin.url)
    if ($remote -match 'github\.com[:/](?<slug>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') {
        return $Matches.slug
    }
    throw "Cannot parse GitHub repository from remote.origin.url: $remote"
}

# Purpose: Resolve the commit SHA to inspect in GitHub Actions.
# Inputs: `Commit` may explicitly provide a SHA; otherwise `HEAD` is used.
# Outputs: Returns a full commit SHA.
function Resolve-WorkflowCommit {
    param([string]$Commit)

    if (-not [string]::IsNullOrWhiteSpace($Commit)) {
        return $Commit
    }
    return (git -C $repoRoot rev-parse HEAD).Trim()
}

# Purpose: Fetch GitHub workflow runs for a commit.
# Inputs: `Repository` is owner/repo and `Commit` is a commit SHA.
# Outputs: Returns parsed run objects from `gh run list`.
function Get-WorkflowRunForCommit {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Commit
    )

    $json = gh run list --repo $Repository --commit $Commit --limit 50 --json databaseId,workflowName,name,status,conclusion,url,updatedAt
    if ([string]::IsNullOrWhiteSpace($json)) {
        return @()
    }
    return @($json | ConvertFrom-Json)
}

# Purpose: Return whether all selected workflow runs have completed successfully.
# Inputs: `Runs` is the GitHub run list and `WorkflowName` is the selected workflow-name set.
# Outputs: Returns an object with completion status and a concise status line.
function Test-SelectedWorkflowCompletion {
    param(
        [object[]]$Runs,
        [string[]]$WorkflowName
    )

    $missing = @()
    $failed = @()
    $running = @()
    foreach ($name in $WorkflowName) {
        $run = @($Runs | Where-Object { $_.workflowName -eq $name } | Select-Object -First 1)
        if ($run.Count -eq 0) {
            $missing += $name
            continue
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

$plan = Get-SuperZipVerificationPlan -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef -SuspectGlobalBug:$Full
$selected = if (@($WorkflowName).Count -gt 0) { @($WorkflowName) } else { @($plan.postPushWorkflows) }
if (@($WorkflowName).Count -eq 0 -and ($IncludeLongRunning.IsPresent -or $FinalCommit.IsPresent)) {
    $selected = @($selected) + @($plan.longRunningPostPushWorkflows)
    $selected = @($selected | Select-Object -Unique)
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
if (@($plan.longRunningPostPushWorkflows).Count -gt 0 -and -not ($IncludeLongRunning.IsPresent -or $FinalCommit.IsPresent) -and @($WorkflowName).Count -eq 0) {
    Write-Output "Long-running workflows are not waited by default: $($plan.longRunningPostPushWorkflows -join ', ')"
    Write-Output "Use -Mode opportunistic -IncludeLongRunning to check them during iteration, or -Mode final -FinalCommit before final handoff/release."
}
if ($Mode -eq "defer") {
    if ($plan.workflowWaitPolicy.immediateRequired -and -not $AllowCriticalDefer.IsPresent) {
        throw "Workflow waiting cannot be deferred for this change. Reasons: $($plan.workflowWaitPolicy.reasons -join '; ')"
    }
    Write-Output "Deferred workflow waiting for iterative development. Final handoff must wait for: $($selected -join ', ')"
    return
}

$repositorySlug = Resolve-GitHubRepository -Repository $Repository
$commitSha = Resolve-WorkflowCommit -Commit $Commit
$deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
if ($Mode -eq "opportunistic") {
    Write-Output ("Checking relevant workflows opportunistically on {0}@{1}: {2}" -f $repositorySlug, $commitSha, ($selected -join ', '))
    $runs = Get-WorkflowRunForCommit -Repository $repositorySlug -Commit $commitSha
    $status = Test-SelectedWorkflowCompletion -Runs $runs -WorkflowName $selected
    if ($status.failed.Count -gt 0) {
        throw "Relevant workflow failure(s): $($status.failed -join '; ')"
    }
    if ($status.complete) {
        Write-Output "Relevant workflows completed successfully: $($selected -join ', ')"
        if ($plan.postPushAuditRequired -and -not $SkipPostPushAudit.IsPresent) {
            & (Join-Path $PSScriptRoot "github_post_push_audit.ps1") -Repository $repositorySlug
        }
        return
    }
    Write-Output "Relevant workflows are not complete yet. Missing: $($status.missing -join ', ') Running: $($status.running -join ', ')"
    Write-Output "Continuing without blocking because Mode=opportunistic. Run this script again with -Mode final before handoff."
    return
}

Write-Output ("Waiting for relevant workflows on {0}@{1}: {2}" -f $repositorySlug, $commitSha, ($selected -join ', '))

while ($true) {
    $runs = Get-WorkflowRunForCommit -Repository $repositorySlug -Commit $commitSha
    $status = Test-SelectedWorkflowCompletion -Runs $runs -WorkflowName $selected
    if ($status.failed.Count -gt 0) {
        throw "Relevant workflow failure(s): $($status.failed -join '; ')"
    }
    if ($status.complete) {
        Write-Output "Relevant workflows completed successfully: $($selected -join ', ')"
        break
    }
    if ([DateTime]::UtcNow -ge $deadline) {
        throw "Timed out waiting for relevant workflows. Missing: $($status.missing -join ', ') Running: $($status.running -join ', ')"
    }
    Write-Output "Still waiting. Missing: $($status.missing -join ', ') Running: $($status.running -join ', ')"
    Start-Sleep -Seconds $PollSeconds
}

if ($plan.postPushAuditRequired -and -not $SkipPostPushAudit.IsPresent) {
    & (Join-Path $PSScriptRoot "github_post_push_audit.ps1") -Repository $repositorySlug
}
