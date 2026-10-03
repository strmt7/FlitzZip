# Purpose: Bind the release action and its workspace scripts to the workflow's exact commit.
# Inputs: RepoRoot contains the owned release workflow.
# Outputs: Throws on action or checkout identity drift.
function Assert-ReleaseWorkflowIdentity {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $workflowText = Get-Content -LiteralPath (Join-Path $RepoRoot '.github/workflows/release.yml') -Raw
    $references = [regex]::Matches($workflowText, '(?m)^\s*uses:\s*(?<reference>\S*windows-release)\s*$')
    if ($references.Count -ne 1 -or $references[0].Groups['reference'].Value -cne '$/.github/actions/windows-release') {
        throw 'Release action must use the self-repository reference bound to the workflow commit.'
    }
    $checkout = [regex]::Match($workflowText, '(?ms)^      - name: Checkout\r?\n(?<body>.*?)(?=^      - |\z)')
    if (-not $checkout.Success -or
        $checkout.Groups['body'].Value -notmatch '(?m)^        uses: actions/checkout@[0-9a-f]{40}(?:\s+#.*)?\s*$' -or
        $checkout.Groups['body'].Value -notmatch '(?m)^          ref: \$\{\{ github\.sha \}\}\s*$' -or
        $checkout.Groups['body'].Value -notmatch '(?m)^          persist-credentials: false\s*$') {
        throw 'Release workspace checkout must use a pinned action, github.sha and disabled credential persistence.'
    }
}

# Purpose: Verify release replacement remains guarded by an explicit version-specific acknowledgement.
# Inputs: Reads the release workflow and composite release action from the repository.
# Outputs: Throws when replacement can delete an existing release/tag without the acknowledgement gate.
function Assert-ReleaseReplacementSafeguard {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $releaseWorkflow = Join-Path $RepoRoot ".github\workflows\release.yml"
    $releaseAction = Join-Path $RepoRoot ".github\actions\windows-release\action.yml"
    if (-not (Test-Path -LiteralPath $releaseWorkflow) -or -not (Test-Path -LiteralPath $releaseAction)) {
        throw "Release workflow and windows-release action must both exist for replacement safeguard validation."
    }

    $workflowText = Get-Content -LiteralPath $releaseWorkflow -Raw
    $actionText = Get-Content -LiteralPath $releaseAction -Raw
    foreach ($requiredSnippet in @(
            "replacement_acknowledgement:",
            'replacement_acknowledgement: ${{ inputs.replacement_acknowledgement }}')) {
        if ($workflowText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Release workflow is missing the replacement acknowledgement safeguard: $requiredSnippet"
        }
    }
    foreach ($requiredSnippet in @(
            "replacement_acknowledgement:",
            "REPLACEMENT_ACKNOWLEDGEMENT",
            "replace_existing=true requires replacement_acknowledgement exactly",
            "REPLACE_RELEASE_TAG=`$releaseTag",
            "Replacement tag tracking mismatch",
            'MsiProductIdentity = "github-run-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT-$env:GITHUB_SHA"',
            "Replacement is exceptional")) {
        if ($actionText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Windows release action is missing the replacement acknowledgement safeguard: $requiredSnippet"
        }
    }
    if ($workflowText -notmatch [regex]::Escape("MSI replacements get a fresh ProductCode from the release run identity")) {
        throw "Release workflow input text must document same-version MSI replacement identity."
    }
}

# Purpose: Verify release notes do not duplicate the GitHub release title as a Markdown H1.
# Inputs: Reads the composite release action that generates `out\release-notes.md`.
# Outputs: Throws when the generated notes would repeat the release title inside the body.
function Assert-ReleaseNotesDoNotDuplicateTitle {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $releaseAction = Join-Path $RepoRoot ".github\actions\windows-release\action.yml"
    if (-not (Test-Path -LiteralPath $releaseAction)) {
        throw "Windows release action must exist for release-note title validation."
    }
    $actionText = Get-Content -LiteralPath $releaseAction -Raw
    if ($actionText -match '#\s*SuperZip\s+\$env:RELEASE_TAG') {
        throw "Release notes must not include a duplicate '# SuperZip `$env:RELEASE_TAG' heading; GitHub already renders the release title."
    }
}
