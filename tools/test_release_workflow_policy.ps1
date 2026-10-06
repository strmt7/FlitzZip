$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release_workflow_policy.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $tempParent ('superzip-release-policy-' + [guid]::NewGuid().ToString('N'))

# Purpose: Exercise a release safeguard against one mutation of the actual owned workflow/action.
# Inputs: FixtureRoot, relative file, original/replacement text, and validator function name.
# Outputs: Throws unless the unmodified file passes and its mutation fails; restores the fixture.
function Assert-ReleasePolicyMutation {
    param([string]$FixtureRoot, [string]$Path, [string]$Original, [string]$Replacement, [string]$Validator)

    $file = Join-Path $FixtureRoot $Path
    $text = Get-Content -LiteralPath $file -Raw
    if (-not $text.Contains($Original)) { throw "Release fixture no longer contains: $Original" }
    & $Validator -RepoRoot $FixtureRoot
    try {
        Set-Content -LiteralPath $file -Value $text.Replace($Original, $Replacement) -Encoding UTF8
        $rejected = $false
        try { & $Validator -RepoRoot $FixtureRoot }
        catch { $rejected = $true }
        if (-not $rejected) { throw "Release safeguard accepted mutation: $Original" }
    } finally {
        Set-Content -LiteralPath $file -Value $text -Encoding UTF8
    }
}

try {
    foreach ($relative in @('.github/workflows/release.yml')) {
        $destination = Join-Path $fixtureRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repoRoot $relative) -Destination $destination
    }
    foreach ($validator in @('Assert-ReleaseWorkflowIdentity', 'Assert-ReleaseReplacementSafeguard',
            'Assert-ReleaseNotesDoNotDuplicateTitle', 'Assert-ReleaseValidationIsolation')) {
        & $validator -RepoRoot $fixtureRoot
    }
    $workflow = '.github/workflows/release.yml'
    foreach ($mutation in @(
            @($workflow, 'uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97', 'uses: ./.github/actions/untrusted', 'Assert-ReleaseWorkflowIdentity'),
            @($workflow, 'uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97', 'uses: $/.github/actions/untrusted', 'Assert-ReleaseWorkflowIdentity'),
            @($workflow, '      - name: Checkout', "      - name: Before checkout`n        run: Write-Output before`n      - name: Checkout", 'Assert-ReleaseWorkflowIdentity'),
            @($workflow, 'ref: ${{ github.sha }}', 'ref: main', 'Assert-ReleaseWorkflowIdentity'),
            @($workflow, 'persist-credentials: false', 'persist-credentials: true', 'Assert-ReleaseWorkflowIdentity'),
            @($workflow, 'REPLACEMENT_ACKNOWLEDGEMENT: ${{ inputs.replacement_acknowledgement }}', 'REPLACEMENT_ACKNOWLEDGEMENT: unused', 'Assert-ReleaseReplacementSafeguard'),
            @($workflow, 'MSI replacements get a fresh ProductCode from the release run identity', 'MSI replacement', 'Assert-ReleaseReplacementSafeguard'),
            @($workflow, 'Replacement tag tracking mismatch', 'Unchecked tag', 'Assert-ReleaseReplacementSafeguard'),
            @($workflow, 'MsiProductIdentity = "github-run-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT-$env:GITHUB_SHA"', 'MsiProductIdentity = "fixed"', 'Assert-ReleaseReplacementSafeguard'),
            @($workflow, 'Replacement is exceptional', '# SuperZip $env:RELEASE_TAG', 'Assert-ReleaseNotesDoNotDuplicateTitle'),
            @($workflow, '    if: inputs.publish_release', '    if: always()', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '      contents: read # validation', '      contents: write # validation', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '    needs: hosted-windows', '    needs: unrelated', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Validated candidate checksum mismatch', 'Unchecked candidate', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Release creation failed', 'Ignored creation status', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Validation-only runs cannot replace an existing release', 'Unchecked replacement mode', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '          name: release-candidate-${{ github.sha }}', '          name: release-candidate-main', 'Assert-ReleaseValidationIsolation'))) {
        Assert-ReleasePolicyMutation -FixtureRoot $fixtureRoot -Path $mutation[0] -Original $mutation[1] `
            -Replacement $mutation[2] -Validator $mutation[3]
    }
    foreach ($relative in @($workflow)) {
        $file = Join-Path $fixtureRoot $relative
        $text = Get-Content -LiteralPath $file -Raw
        Remove-Item -LiteralPath $file
        $rejected = $false
        try { Assert-ReleaseReplacementSafeguard -RepoRoot $fixtureRoot }
        catch { $rejected = $true }
        if (-not $rejected) { throw "Missing release policy input was accepted: $relative" }
        Set-Content -LiteralPath $file -Value $text -Encoding UTF8
    }
    Write-Output 'Release workflow identity, replacement, title and candidate-isolation contracts passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $parentPrefix = $tempParent.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^superzip-release-policy-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the owned release policy fixture.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
