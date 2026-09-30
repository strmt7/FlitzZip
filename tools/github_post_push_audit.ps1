param(
    [string]$Repository = "",
    [switch]$IncludeHistory,
    [string]$HistoryReportPath = ""
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
    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot resolve GitHub repository because reading remote.origin.url failed.'
    }
    if ([string]::IsNullOrWhiteSpace($remote)) {
        throw "Cannot resolve GitHub repository because remote.origin.url is unset."
    }
    if ($remote -is [string] -and
        $remote -match '^(https://github\.com/|ssh://git@github\.com/|git@github\.com:)(?<slug>[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') {
        return $Matches.slug
    }
    throw 'Cannot parse GitHub repository from remote.origin.url; use a credential-free GitHub remote or explicit owner/repo.'
}

# Purpose: Fetch a complete open-alert or all-state code-scanning inventory.
# Inputs: Repository slug, validated page size, and whether resolved/dismissed records are required.
# Outputs: Returns alert objects; rejects unavailable, malformed, duplicate, or unexpected-state evidence.
function Get-CodeScanningAlert {
    param(
        [string]$Repository,
        [ValidateRange(1, 100)][int]$PageSize = 100,
        [switch]$IncludeHistory
    )

    $alerts = [System.Collections.Generic.List[object]]::new()
    $seen = [System.Collections.Generic.HashSet[long]]::new()
    $stateQuery = if ($IncludeHistory) { "" } else { "state=open&" }
    $page = 1
    do {
        $json = gh api "repos/$Repository/code-scanning/alerts?${stateQuery}per_page=$PageSize&page=$page"
        if ($LASTEXITCODE -ne 0) {
            throw "GitHub code-scanning API failed on page $page (exit code $LASTEXITCODE); findings are unavailable."
        }
        if ([string]::IsNullOrWhiteSpace(($json -join "`n"))) {
            throw "GitHub returned an empty code-scanning response on page $page."
        }
        try {
            $parameters = @{ InputObject = ($json -join "`n") }
            # Windows PowerShell preserves the root array; newer PowerShell needs this explicit option.
            if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('NoEnumerate')) {
                $parameters.NoEnumerate = $true
            }
            $document = @(ConvertFrom-Json @parameters)
        } catch {
            throw "GitHub returned invalid code-scanning JSON on page $page."
        }
        if ($document.Count -ne 1 -or $document[0] -isnot [System.Array]) {
            throw "GitHub returned a non-array code-scanning response on page $page."
        }
        $items = $document[0]
        if ($items.Count -gt $PageSize) {
            throw "GitHub returned more records than requested on page $page."
        }
        foreach ($item in $items) {
            if ($item -isnot [pscustomobject] -or
                ($item.number -isnot [int] -and $item.number -isnot [long]) -or $item.number -le 0 -or
                $item.tool.name -isnot [string] -or [string]::IsNullOrWhiteSpace($item.tool.name) -or
                $item.rule.id -isnot [string] -or [string]::IsNullOrWhiteSpace($item.rule.id) -or
                $item.state -isnot [string] -or
                $item.state -notin @('open', 'fixed', 'dismissed')) {
                throw "GitHub returned an invalid code-scanning record on page $page."
            }
            if (-not $IncludeHistory -and $item.state -ne 'open') {
                throw "GitHub returned a non-open alert in the open-only inventory on page $page."
            }
            if (-not $seen.Add([long]$item.number)) {
                throw "GitHub returned duplicate alert $($item.number) on page $page; pagination changed or repeated."
            }
            $alerts.Add($item)
        }
        $page += 1
    } while ($items.Count -eq $PageSize)
    return $alerts.ToArray()
}

# Purpose: Summarize every retrieved incident without treating prior closure as current remediation proof.
# Inputs: Repository slug and complete, validated all-state alert inventory.
# Outputs: Returns a schema-versioned report with rule/state counts and minimal per-incident evidence.
function Get-CodeScanningHistoryReport {
    param([string]$Repository, [object[]]$Alerts)

    $rules = @($Alerts | Group-Object { "$($_.tool.name)`t$($_.rule.id)" } | Sort-Object Name | ForEach-Object {
        $group = $_.Group
        [ordered]@{
            tool = $group[0].tool.name
            rule = $group[0].rule.id
            open = @($group | Where-Object state -eq 'open').Count
            fixed = @($group | Where-Object state -eq 'fixed').Count
            dismissed = @($group | Where-Object state -eq 'dismissed').Count
        }
    })
    $incidents = @($Alerts | Sort-Object number | ForEach-Object {
        [ordered]@{
            number = $_.number
            tool = $_.tool.name
            tool_version = $_.tool.version
            rule = $_.rule.id
            severity = $_.rule.severity
            security_severity = $_.rule.security_severity_level
            state = $_.state
            path = $_.most_recent_instance.location.path
            line = $_.most_recent_instance.location.start_line
            analysis_commit = $_.most_recent_instance.commit_sha
            analysis_category = $_.most_recent_instance.category
            dismissed_reason = $_.dismissed_reason
        }
    })
    return [ordered]@{
        schema_version = 1
        repository = $Repository
        retrieved_utc = [DateTime]::UtcNow.ToString('o')
        total = $Alerts.Count
        open = @($Alerts | Where-Object state -eq 'open').Count
        fixed = @($Alerts | Where-Object state -eq 'fixed').Count
        dismissed = @($Alerts | Where-Object state -eq 'dismissed').Count
        rules = $rules
        incidents = $incidents
    }
}

# Purpose: Preserve reusable incident evidence without overwriting a prior report or storing raw credentials.
# Inputs: New JSON report path and minimal, schema-versioned history report.
# Outputs: Creates parent directories and a UTF-8 report exclusively; throws on collisions or write failure.
function Write-CodeScanningHistoryReport {
    param([string]$Path, [System.Collections.IDictionary]$Report)

    if ([IO.Path]::GetExtension($Path) -ne '.json') {
        throw 'Code-scanning history reports must use a .json extension.'
    }
    $fullPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fullPath))
    try {
        $stream = [IO.File]::Open($fullPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    } catch [IO.IOException] {
        if ([IO.File]::Exists($fullPath)) {
            throw 'Code-scanning history report already exists; choose a new path.'
        }
        throw
    }
    try {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        try {
            $writer.WriteLine(($Report | ConvertTo-Json -Depth 8))
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    Write-Output "Code-scanning history report saved: $fullPath"
}

# Purpose: Verify that no workflow has created GitHub deployment records.
# Inputs: `Repository` is an `owner/repo` slug.
# Outputs: Throws when any deployment record exists or the API count cannot be verified.
function Assert-NoDeployment {
    param([string]$Repository)

    $countText = gh api "repos/$Repository/deployments" --jq 'length'
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub deployments API failed (exit code $LASTEXITCODE); deployment state is unavailable."
    }
    $count = 0
    if (-not [int]::TryParse([string]$countText, [ref]$count) -or $count -lt 0) {
        throw "GitHub returned an invalid deployment count."
    }
    if ($count -ne 0) {
        throw "GitHub deployments are forbidden in this repository, but $count deployment record(s) exist."
    }
}

# Purpose: Verify that open code-scanning alerts are limited to approved residual Scorecard findings.
# Inputs: `Alerts` contains validated open alerts only, never resolved or dismissed history.
# Outputs: Throws with full grouped counts and bounded examples when an unapproved alert is open.
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
        $groups = $violations | Group-Object { "$($_.tool.name)/$($_.rule.id)" } | Sort-Object Name | ForEach-Object {
            "$($_.Count) $($_.Name)"
        }
        $priority = @{
            Expression = {
                switch ($_.rule.security_severity_level) {
                    'critical' { 0 }
                    'high' { 1 }
                    'medium' { 2 }
                    'low' { 3 }
                    default { 4 }
                }
            }
        }
        $details = $violations | Sort-Object $priority, @{ Expression = 'number'; Descending = $true } |
            Select-Object -First 12 | ForEach-Object {
            $path = if ($_.most_recent_instance.location.path) { $_.most_recent_instance.location.path } else { "no file" }
            "$($_.number) $($_.tool.name)/$($_.rule.id) $path"
        }
        throw "Unapproved code-scanning alerts are open: $($violations.Count).`nBy rule:`n$($groups -join "`n")`nFirst 12 examples (at most):`n$($details -join "`n")"
    }
}

if ($HistoryReportPath -and -not $IncludeHistory) {
    throw 'HistoryReportPath requires IncludeHistory; an open-only snapshot is not a complete incident review.'
}
$repo = Resolve-GitHubRepository -Repository $Repository
Assert-NoDeployment -Repository $repo
$inventory = @(Get-CodeScanningAlert -Repository $repo -IncludeHistory:$IncludeHistory)
if ($IncludeHistory) {
    $report = Get-CodeScanningHistoryReport -Repository $repo -Alerts $inventory
    Write-Output "Code-scanning history: $($report.total) records; open=$($report.open), fixed=$($report.fixed), dismissed=$($report.dismissed); tool/rule groups=$($report.rules.Count)."
    if ($HistoryReportPath) {
        Write-CodeScanningHistoryReport -Path $HistoryReportPath -Report $report
    }
}
$alerts = @($inventory | Where-Object state -eq 'open')
Assert-CodeScanningAllowList -Alerts $alerts
Write-Output "GitHub post-push audit passed for $repo. Deployments: 0. Open code-scanning alerts: $($alerts.Count)."
