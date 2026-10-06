param(
    [string]$Repository = "",
    [string]$Commit = "",
    [switch]$IncludeHistory,
    [string]$HistoryReportPath = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'github_repository.ps1')

# Purpose: Preserve JSON array roots consistently across Windows PowerShell and newer PowerShell.
# Inputs: Bounded GitHub response text and a caller-owned diagnostic label.
# Outputs: One array object, including empty/null entries; malformed or non-array responses fail closed.
function ConvertFrom-GitHubArray {
    param([string]$Text, [string]$Label)

    if ($Text.Length -gt 16MB) { throw "GitHub $Label response exceeds its character budget." }
    try {
        $parameters = @{ InputObject = $Text }
        if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('NoEnumerate')) { $parameters.NoEnumerate = $true }
        $document = @(ConvertFrom-Json @parameters)
    } catch {
        throw "GitHub returned invalid $Label JSON."
    }
    if ($document.Count -ne 1 -or $document[0] -isnot [System.Array]) {
        throw "GitHub returned a non-array $Label response."
    }
    return ,$document[0]
}

# Purpose: Fetch a complete selected-state or all-state code-scanning inventory.
# Inputs: Repository slug, page size, requested state, and whether complete incident history is required.
# Outputs: Returns alert objects; rejects unavailable, malformed, duplicate, or unexpected-state evidence.
function Get-CodeScanningAlert {
    param(
        [string]$Repository,
        [ValidateRange(1, 100)][int]$PageSize = 100,
        [ValidateSet('open', 'dismissed')][string]$State = 'open',
        [switch]$IncludeHistory
    )

    $alerts = [System.Collections.Generic.List[object]]::new()
    $seen = [System.Collections.Generic.HashSet[long]]::new()
    $stateQuery = if ($IncludeHistory) { "" } else { "state=$State&" }
    $page = 1
    do {
        $json = gh api "repos/$Repository/code-scanning/alerts?${stateQuery}per_page=$PageSize&page=$page"
        if ($LASTEXITCODE -ne 0) {
            throw "GitHub code-scanning API failed on page $page (exit code $LASTEXITCODE); findings are unavailable."
        }
        if ([string]::IsNullOrWhiteSpace(($json -join "`n"))) {
            throw "GitHub returned an empty code-scanning response on page $page."
        }
        $items = ConvertFrom-GitHubArray -Text ($json -join "`n") -Label 'code-scanning'
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
            if (-not $IncludeHistory -and $item.state -ne $State) {
                throw "GitHub returned a non-$State alert in the $State-only inventory on page $page."
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

# Purpose: Require closure without inheriting a historical dismissal for a reproduced source finding.
# Inputs: Validated open alerts and current dismissed findings after exact public-metadata review.
# Outputs: Throws with grouped counts and bounded examples for either unresolved inventory.
function Assert-CodeScanningClosure {
    param([object[]]$Alerts, [object[]]$CurrentDismissed = @())

    $violations = @($Alerts) + @($CurrentDismissed)
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
        throw "Unresolved code-scanning alerts are open: $($Alerts.Count). Active dismissed findings: $($CurrentDismissed.Count).`nBy rule:`n$($groups -join "`n")`nFirst 12 examples (at most):`n$($details -join "`n")"
    }
}

# Purpose: Apply only the existing exact public-integrity contract to current dismissed metadata reports.
# Inputs: Current dismissed alert objects and a full analyzed commit; no source-review ledger is consulted.
# Outputs: Returns individually matched metadata IDs; unavailable or malformed review evidence fails closed.
function Get-CurrentMetadataReview {
    param([object[]]$Alerts, [string]$Commit)

    if (@($Alerts | Where-Object { $_.tool.name -eq 'devskim' -and $_.rule.id -eq 'DS173237' }).Count -eq 0) {
        return @()
    }
    $directory = Join-Path $repoRoot ('out/post-push-audit/' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($directory)
    $snapshot = Join-Path $directory 'current-dismissed.json'
    $minimal = @($Alerts | ForEach-Object {
        [ordered]@{ number = $_.number; state = $_.state; tool = $_.tool; rule = $_.rule
            most_recent_instance = $_.most_recent_instance }
    })
    [IO.File]::WriteAllText($snapshot, (ConvertTo-Json -InputObject $minimal -Depth 12), [Text.UTF8Encoding]::new($false))
    Push-Location $repoRoot
    try {
        $output = py -3 -B -m tools.scanner_metadata_review --hosted-alerts $snapshot --commit $Commit --current-metadata
        if ($LASTEXITCODE -ne 0) { throw 'Current public-metadata review failed; source findings remain blocking.' }
        $review = ($output -join "`n") | ConvertFrom-Json
        if ($review.PSObject.Properties.Name.Count -ne 1 -or
            $review.PSObject.Properties.Name -ne 'reviewed_metadata' -or
            $review.reviewed_metadata -isnot [System.Array]) {
            throw 'Current public-metadata review returned an invalid decision.'
        }
        $seen = [System.Collections.Generic.HashSet[long]]::new()
        $possible = @($Alerts.number)
        foreach ($number in $review.reviewed_metadata) {
            if (($number -isnot [int] -and $number -isnot [long]) -or $number -le 0 -or
                $number -notin $possible -or -not $seen.Add([long]$number)) {
                throw 'Current public-metadata review returned an unbound decision.'
            }
        }
        return @($review.reviewed_metadata)
    } finally {
        Pop-Location
    }
}

# Purpose: Bind reproduced reports to the latest complete default-branch source analyses, including valid reuse.
# Inputs: Repository and requested commit; CodeQL reuse requires unchanged native/query inputs at checkout HEAD.
# Outputs: Latest analysis commits by producer/category; missing, stale or malformed evidence fails closed.
function Get-CurrentSourceAnalysis {
    param([string]$Repository, [string]$Commit)

    $branch = gh api "repos/$Repository" --jq '.default_branch'
    if ($LASTEXITCODE -ne 0 -or $branch -notmatch '^[A-Za-z0-9._/-]+$') {
        throw 'Cannot resolve the default branch for source analysis.'
    }
    $reference = [Uri]::EscapeDataString("refs/heads/$branch")
    $required = @('CodeQL:/language:c-cpp', 'CodeQL:/language:c-cpp/configuration:hip-host', 'devskim:devskim')
    $latest = @{}
    for ($page = 1; $page -le 32 -and $latest.Count -lt $required.Count; ++$page) {
        $json = gh api "repos/$Repository/code-scanning/analyses?ref=$reference&per_page=100&page=$page&direction=desc" `
            --jq '[.[] | {tool: .tool.name, category, commit_sha, error, warning}]'
        if ($LASTEXITCODE -ne 0) { throw 'GitHub source-analysis inventory failed.' }
        if (-not ($json -join "`n").TrimStart().StartsWith('[')) { throw 'Invalid source-analysis inventory.' }
        $records = ConvertFrom-GitHubArray -Text ($json -join "`n") -Label 'source-analysis'
        if ($records.Count -gt 100) { throw 'Source-analysis inventory exceeds its page bound.' }
        foreach ($record in $records) {
            if ($record -isnot [pscustomobject] -or $record.tool -isnot [string] -or
                $record.category -isnot [string] -or $record.commit_sha -cnotmatch '^[0-9a-f]{40}$') {
                throw 'Invalid source-analysis identity.'
            }
            $key = "$($record.tool):$($record.category)"
            if ($key -in $required -and -not $latest.ContainsKey($key)) {
                if ($record.error -isnot [string] -or $record.error -ne '' -or
                    $record.warning -isnot [string] -or $record.warning -ne '') {
                    throw 'Source analysis contains errors or incomplete-coverage warnings.'
                }
                $latest[$key] = $record.commit_sha
            }
        }
        if ($records.Count -lt 100) { break }
    }
    if ($latest.Count -ne $required.Count) { throw 'Complete CPU, HIP and DevSkim source analyses are unavailable.' }
    if ($latest['devskim:devskim'] -ne $Commit) { throw 'DevSkim analysis does not cover the requested commit.' }
    $checked = @{}
    foreach ($key in $required | Where-Object { $_.StartsWith('CodeQL:') }) {
        $analyzedCommit = $latest[$key]
        if ($analyzedCommit -eq $Commit -or $checked.ContainsKey($analyzedCommit)) { continue }
        $checkoutCommit = git -C $repoRoot rev-parse --verify 'HEAD^{commit}'
        if ($LASTEXITCODE -ne 0 -or $checkoutCommit -cne $Commit) {
            throw 'CodeQL reuse requires the requested commit at checkout HEAD.'
        }
        Push-Location $repoRoot
        try {
            $output = py -3 -B -m tools.cpp_security_plan --event push --push-base $analyzedCommit
            if ($LASTEXITCODE -ne 0) { throw 'Cannot establish CodeQL source/configuration equivalence.' }
            $plan = ($output -join "`n") | ConvertFrom-Json
            if ($plan.codeql_cpp -isnot [bool] -or $plan.codeql_hip_host -isnot [bool] -or
                $plan.whole_database -isnot [bool] -or -not $plan.whole_database -or
                $plan.codeql_cpp -or $plan.codeql_hip_host) {
                throw 'CodeQL analysis is stale for changed native/query inputs.'
            }
        } finally {
            Pop-Location
        }
        $checked[$analyzedCommit] = $true
    }
    return $latest
}

if ($HistoryReportPath -and -not $IncludeHistory) {
    throw 'HistoryReportPath requires IncludeHistory; an open-only snapshot is not a complete incident review.'
}
$repo = Resolve-GitHubRepository -Repository $Repository -RepositoryRoot $repoRoot
if ($Commit -and $Commit -cnotmatch '^[0-9a-f]{40}$') { throw 'Post-push audit requires a full commit SHA.' }
$candidate = if ($Commit) { $Commit } else { 'HEAD' }
$resolvedCommit = git -C $repoRoot rev-parse --verify "$candidate^{commit}"
if ($LASTEXITCODE -ne 0 -or $resolvedCommit -cnotmatch '^[0-9a-f]{40}$' -or
    ($Commit -and $Commit -cne $resolvedCommit)) {
    throw 'Cannot resolve the requested checkout commit for current finding review.'
}
$Commit = $resolvedCommit
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
$dismissed = if ($IncludeHistory) { @($inventory | Where-Object state -eq 'dismissed') }
else { @(Get-CodeScanningAlert -Repository $repo -State dismissed) }
$analyses = Get-CurrentSourceAnalysis -Repository $repo -Commit $Commit
$current = @($dismissed | Where-Object {
    $key = "$($_.tool.name):$($_.most_recent_instance.category)"
    $analysisCommit = if ($analyses.ContainsKey($key)) { $analyses[$key] } else { $Commit }
    $_.most_recent_instance.commit_sha -eq $analysisCommit -and $_.most_recent_instance.state -eq 'dismissed'
})
$metadata = @(Get-CurrentMetadataReview -Alerts $current -Commit $Commit)
$unresolved = @($current | Where-Object { $_.number -notin $metadata })
Assert-CodeScanningClosure -Alerts $alerts -CurrentDismissed $unresolved
Write-Output "GitHub post-push audit passed for $repo. Deployments: 0. Open alerts: 0. Active dismissed findings: 0. Exact public-metadata reviews: $($metadata.Count). Commit: $Commit."
