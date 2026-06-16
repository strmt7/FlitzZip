param(
    [string]$Repository = ""
)

$ErrorActionPreference = "Stop"

# Purpose: Resolve the GitHub `owner/repo` slug for the current checkout.
# Inputs: `Repository` may explicitly provide `owner/repo`; otherwise the function reads `remote.origin.url`.
# Outputs: Returns an `owner/repo` string or throws when the remote cannot be parsed.
function Resolve-GitHubRepository {
    param([string]$Repository)

    if (-not [string]::IsNullOrWhiteSpace($Repository)) {
        if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
            throw "Repository must use owner/repo form."
        }
        return $Repository
    }

    $remote = (git config --get remote.origin.url)
    if ([string]::IsNullOrWhiteSpace($remote)) {
        throw "Cannot resolve GitHub repository because remote.origin.url is unset."
    }
    if ($remote -match 'github\.com[:/](?<slug>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') {
        return $Matches.slug
    }
    throw "Cannot parse GitHub repository from remote.origin.url: $remote"
}

# Purpose: Fetch all open GitHub code-scanning alerts for a repository.
# Inputs: `Repository` is an `owner/repo` slug and `PageSize` controls GitHub API pagination.
# Outputs: Returns an array of alert objects.
function Get-OpenCodeScanningAlerts {
    param(
        [string]$Repository,
        [int]$PageSize = 100
    )

    $alerts = @()
    $page = 1
    do {
        $json = gh api "repos/$Repository/code-scanning/alerts?state=open&per_page=$PageSize&page=$page"
        $items = @($json | ConvertFrom-Json)
        $alerts += $items
        $page += 1
    } while ($items.Count -eq $PageSize)
    return $alerts
}

# Purpose: Verify that no workflow has created GitHub deployment records.
# Inputs: `Repository` is an `owner/repo` slug.
# Outputs: Throws when any deployment record exists.
function Assert-NoDeployments {
    param([string]$Repository)

    $countText = gh api "repos/$Repository/deployments" --jq 'length'
    $count = [int]$countText
    if ($count -ne 0) {
        throw "GitHub deployments are forbidden in this repository, but $count deployment record(s) exist."
    }
}

# Purpose: Verify that open code-scanning alerts are limited to approved residual Scorecard findings.
# Inputs: `Alerts` is the array returned by `Get-OpenCodeScanningAlerts`.
# Outputs: Throws with a concise report when an unapproved alert is open.
function Assert-CodeScanningAllowList {
    param([object[]]$Alerts)

    $allowedScorecardRules = @(
        "MaintainedID",
        "CodeReviewID",
        "BranchProtectionID",
        "CIIBestPracticesID"
    )

    $violations = @($Alerts | Where-Object {
        -not ($_.tool.name -eq "Scorecard" -and $allowedScorecardRules -contains $_.rule.id)
    })
    if ($violations.Count -gt 0) {
        $details = $violations | ForEach-Object {
            $path = if ($_.most_recent_instance.location.path) { $_.most_recent_instance.location.path } else { "no file" }
            "$($_.number) $($_.tool.name)/$($_.rule.id) $path"
        }
        throw "Unapproved code-scanning alerts are open:`n$($details -join "`n")"
    }
}

$repo = Resolve-GitHubRepository -Repository $Repository
Assert-NoDeployments -Repository $repo
$alerts = Get-OpenCodeScanningAlerts -Repository $repo
Assert-CodeScanningAllowList -Alerts $alerts
Write-Host "GitHub post-push audit passed for $repo. Deployments: 0. Open code-scanning alerts: $($alerts.Count)."
