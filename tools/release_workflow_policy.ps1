# Purpose: Keep release stages in workflow source and bind their workspace scripts to its exact commit.
# Inputs: RepoRoot contains the owned release workflow.
# Outputs: Throws on a local action reference or checkout identity drift.
function Assert-ReleaseWorkflowIdentity {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $workflowText = Get-Content -LiteralPath (Join-Path $RepoRoot '.github/workflows/release.yml') -Raw
    if ($workflowText -match '(?m)^\s*uses:\s*(\./|\$/)') {
        throw 'Release stages must be declared directly in the workflow without a local action reference.'
    }
    $firstStep = [regex]::Match($workflowText, '(?m)^      - name: (?<name>[^\r\n]+)\r?$')
    $checkout = [regex]::Match($workflowText, '(?ms)^      - name: Checkout\r?\n(?<body>.*?)(?=^      - |\z)')
    if (-not $firstStep.Success -or $firstStep.Groups['name'].Value -ne 'Checkout' -or -not $checkout.Success -or
        $checkout.Groups['body'].Value -notmatch '(?m)^        uses: actions/checkout@[0-9a-f]{40}(?:\s+#.*)?\s*$' -or
        $checkout.Groups['body'].Value -notmatch '(?m)^          ref: \$\{\{ github\.sha \}\}\s*$' -or
        $checkout.Groups['body'].Value -notmatch '(?m)^          persist-credentials: false\s*$') {
        throw 'Release workspace checkout must use a pinned action, github.sha and disabled credential persistence.'
    }
}

# Purpose: Verify release replacement remains guarded by an explicit version-specific acknowledgement.
# Inputs: RepoRoot contains the workflow's inputs and release stages.
# Outputs: Throws when replacement can delete an existing release/tag without the acknowledgement gate.
function Assert-ReleaseReplacementSafeguard {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $releaseWorkflow = Join-Path $RepoRoot ".github\workflows\release.yml"
    if (-not (Test-Path -LiteralPath $releaseWorkflow)) {
        throw 'Release workflow must exist for replacement safeguard validation.'
    }

    $workflowText = Get-Content -LiteralPath $releaseWorkflow -Raw
    foreach ($requiredSnippet in @(
            "replacement_acknowledgement:",
            'REPLACEMENT_ACKNOWLEDGEMENT: ${{ inputs.replacement_acknowledgement }}')) {
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
        if ($workflowText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Release stages are missing the replacement acknowledgement safeguard: $requiredSnippet"
        }
    }
    if ($workflowText -notmatch [regex]::Escape("MSI replacements get a fresh ProductCode from the release run identity")) {
        throw "Release workflow input text must document same-version MSI replacement identity."
    }
}

# Purpose: Verify release notes do not duplicate the GitHub release title as a Markdown H1.
# Inputs: Reads the workflow that generates `out\release-notes.md` under RepoRoot.
# Outputs: Throws when the generated notes would repeat the release title inside the body.
function Assert-ReleaseNotesDoNotDuplicateTitle {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $releaseWorkflow = Join-Path $RepoRoot '.github/workflows/release.yml'
    if (-not (Test-Path -LiteralPath $releaseWorkflow)) {
        throw 'Release workflow must exist for release-note title validation.'
    }
    $workflowText = Get-Content -LiteralPath $releaseWorkflow -Raw
    if ($workflowText -match '#\s*SuperZip\s+\$env:RELEASE_TAG') {
        throw "Release notes must not include a duplicate '# SuperZip `$env:RELEASE_TAG' heading; GitHub already renders the release title."
    }
}
