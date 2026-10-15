$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release_workflow_policy.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $tempParent ('flitzzip-release-policy-' + [guid]::NewGuid().ToString('N'))

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

# Purpose: Build a small source-offering fixture from real tracked materials without changing those files.
# Inputs: Destination ZIP, member paths, and an optional missing, altered or duplicate member.
# Outputs: A bounded fixture ZIP used to exercise source-offering rejection and success paths.
function Write-SourceOfferingFixture {
    param([string]$Destination, [string[]]$Member, [string]$Missing = '',
        [string]$Altered = '', [string]$Duplicate = '')

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($relative in $Member) {
            if ($relative -eq $Missing) { continue }
            $copies = if ($relative -eq $Duplicate) { 2 } else { 1 }
            for ($copy = 0; $copy -lt $copies; $copy++) {
                $source = if ($relative -eq $Altered) { 'LICENSE' } else { $relative }
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, (Join-Path $repoRoot $source), ('FlitzZip-source/' + $relative)) | Out-Null
            }
        }
    } finally { $zip.Dispose() }
}

# Purpose: Exercise the workflow's actual MSI invocation against Windows Installer without installing anything.
# Inputs: RepoRoot contains the workflow; FixtureRoot owns temporary scripts and a deliberately absent MSI path.
# Outputs: Throws unless native argument serialization reaches MSI, produces a log and returns package error 1619.
function Assert-ReleaseMsiInvocation {
    param([string]$RepoRoot, [string]$FixtureRoot)

    $workflowText = Get-Content -LiteralPath (Join-Path $RepoRoot '.github/workflows/release.yml') -Raw
    $step = [regex]::Match($workflowText, '(?ms)^      - name: MSI install smoke\r?\n.*?        run: \|\r?\n(?<script>.*?)(?=^      - name:|\z)')
    if (-not $step.Success) { throw 'MSI smoke workflow script was not found.' }
    $scriptText = [regex]::Replace($step.Groups['script'].Value, '(?m)^          ', '')
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($scriptText, [ref]$null, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) { throw 'MSI smoke workflow script does not parse.' }
    $functions = @($ast.FindAll({
                param($node)
                $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                    $node.Name -in @('Write-MsiLogTail', 'Invoke-CheckedMsiExec')
            }, $false))
    if ($functions.Count -ne 2) { throw 'Expected the two production MSI invocation functions.' }
    $probe = Join-Path $FixtureRoot 'native MSI argument probe.ps1'
    $log = Join-Path $FixtureRoot 'native MSI argument probe.log'
    $missingMsi = Join-Path $FixtureRoot 'missing package with spaces.msi'
    if (Test-Path -LiteralPath $missingMsi) { throw 'MSI argument probe requires an absent package.' }
    $prefix = @'
param([string]$MissingMsi, [string]$LogPath)
$ErrorActionPreference = 'Stop'
'@
    $suffix = @'
$failure = ''
try {
    Invoke-CheckedMsiExec -Phase 'MSI argument probe' -TimeoutSeconds 10 -LogPath $LogPath `
        -Arguments @('/i', $MissingMsi, '/qn', '/norestart', '/L*v', $LogPath)
} catch { $failure = $_.Exception.Message }
if ($failure -ne 'MSI argument probe failed with exit code 1619.') {
    throw "The absent package did not return the expected native MSI error: $failure"
}
if (-not (Test-Path -LiteralPath $LogPath) -or (Get-Item -LiteralPath $LogPath).Length -eq 0) {
    throw 'Windows Installer did not produce the requested verbose log.'
}
'@
    $productionFunctions = ($functions | ForEach-Object { $_.Extent.Text }) -join "`n"
    Set-Content -LiteralPath $probe -Value ($prefix + "`n" + $productionFunctions + "`n" + $suffix) -Encoding UTF8
    # The release step uses PowerShell 7/.NET, including ProcessStartInfo.ArgumentList.
    & pwsh -NoLogo -NoProfile -NonInteractive -File $probe -MissingMsi $missingMsi -LogPath $log
    if ($LASTEXITCODE -ne 0) { throw 'The production MSI invocation failed its native argument regression.' }
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
            @($workflow, 'Replacement is exceptional', '# FlitzZip $env:RELEASE_TAG', 'Assert-ReleaseNotesDoNotDuplicateTitle'),
            @($workflow, '    if: inputs.publish_release', '    if: always()', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '      contents: read # validation', '      contents: write # validation', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '    needs: hosted-windows', '    needs: unrelated', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Validated candidate checksum mismatch', 'Unchecked candidate', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Release creation failed', 'Ignored creation status', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Validation-only runs cannot replace an existing release', 'Unchecked replacement mode', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '          name: release-candidate-${{ github.sha }}', '          name: release-candidate-main', 'Assert-ReleaseValidationIsolation'),
            @($workflow, '-WorkRoot $env:RUNNER_TEMP -CliPath $cli', '-WorkRoot $env:RUNNER_TEMP', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Packaged native compression failed.', 'Ignored compression failure.', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Packaged native verification failed.', 'Ignored verification failure.', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Packaged native extraction failed.', 'Ignored extraction failure.', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Packaged native readback differs from the complete source.', 'Unchecked readback.', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'Remove-Item -LiteralPath $expectedCli -Force', 'Write-Output intact', 'Assert-ReleaseValidationIsolation'),
            @($workflow, 'MSI repair did not restore the exact staged CLI bytes.', 'Unchecked repair.', 'Assert-ReleaseValidationIsolation'))) {
        Assert-ReleasePolicyMutation -FixtureRoot $fixtureRoot -Path $mutation[0] -Original $mutation[1] `
            -Replacement $mutation[2] -Validator $mutation[3]
    }
    Assert-ReleasePolicyMutation -FixtureRoot $fixtureRoot -Path $workflow `
        -Original '--output=$source $env:GITHUB_SHA' -Replacement '--output=$source main' `
        -Validator 'Assert-ReleaseValidationIsolation'
    Assert-ReleasePolicyMutation -FixtureRoot $fixtureRoot -Path $workflow `
        -Original '"out\$env:PACKAGE_BASE-source.zip"' -Replacement '"out\missing.zip"' `
        -Validator 'Assert-ReleaseValidationIsolation'
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
    $missingCli = Join-Path $fixtureRoot 'missing-package-cli.exe'
    $missingRejected = $false
    try { & (Join-Path $repoRoot 'tools/format_matrix_smoke.ps1') -CliPath $missingCli -WorkRoot $fixtureRoot }
    catch { $missingRejected = $_.Exception.Message.StartsWith("CLI binary not found: $missingCli.") }
    if (-not $missingRejected) { throw 'An absent packaged CLI must not substitute the development binary.' }
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'docs'), (Join-Path $fixtureRoot 'retired') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureRoot 'docs/current.md') -Value 'Current package documentation.'
    $fixtureCmake = Join-Path $fixtureRoot 'CMakeLists.txt'
    Set-Content -LiteralPath $fixtureCmake -Value 'install(DIRECTORY docs DESTINATION .)'
    Assert-ReleaseSourceDirectory -RepoRoot $fixtureRoot -TrackedPath @('docs/current.md')
    foreach ($inventory in @(@('docs/current.md'), @('docs/current.md', 'retired/missing.md'))) {
        Set-Content -LiteralPath $fixtureCmake -Value 'install(DIRECTORY docs retired DESTINATION .)'
        $staleRejected = $false
        try { Assert-ReleaseSourceDirectory -RepoRoot $fixtureRoot -TrackedPath $inventory }
        catch { $staleRejected = $_.Exception.Message -eq 'Installed source directory contains no existing tracked files: retired' }
        if (-not $staleRejected) { throw 'An empty local directory must not conceal a stale package install rule.' }
    }
    $sourceMembers = @('LICENSE', 'CMakeLists.txt', 'tools/build.ps1', 'tools/package.ps1',
        'docs/third-party.md', 'third_party/wimlib/COPYING.txt', 'third_party/wimlib/COPYING.GPLv3.txt',
        'third_party/wimlib/COPYING.LGPL.txt', 'third_party/upstream/wimlib/1.14.5/wimlib-1.14.5.tar.gz',
        'third_party/upstream/wimlib/1.14.5/SHA256SUMS')
    $sourceFixture = Join-Path $fixtureRoot 'source.zip'
    Write-SourceOfferingFixture -Destination $sourceFixture -Member $sourceMembers
    Assert-ReleaseCorrespondingSource -ArchivePath $sourceFixture -RepoRoot $repoRoot -TrackedPath $sourceMembers
    foreach ($defect in @('Missing', 'Altered', 'Duplicate')) {
        Remove-Item -LiteralPath $sourceFixture -Force
        $options = @{ Destination = $sourceFixture; Member = $sourceMembers }
        $options[$defect] = 'third_party/upstream/wimlib/1.14.5/wimlib-1.14.5.tar.gz'
        Write-SourceOfferingFixture @options
        $rejected = $false
        try { Assert-ReleaseCorrespondingSource -ArchivePath $sourceFixture -RepoRoot $repoRoot -TrackedPath $sourceMembers }
        catch { $rejected = $_.Exception.Message -like 'Corresponding source ZIP*' }
        if (-not $rejected) { throw "Source offering accepted $defect upstream source." }
    }
    Assert-ReleaseMsiInvocation -RepoRoot $repoRoot -FixtureRoot $fixtureRoot
    Assert-ReleaseSourceDirectory -RepoRoot $repoRoot
    Write-Output 'Release workflow identity, replacement, title and candidate-isolation contracts passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $parentPrefix = $tempParent.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^flitzzip-release-policy-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the owned release policy fixture.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
