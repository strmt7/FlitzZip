$ErrorActionPreference = 'Stop'
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$repo = Join-Path $temporaryRoot ('flitzzip-policy-scan-' + [Guid]::NewGuid().ToString('N'))
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'security_scan.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Policy scanner contains PowerShell syntax errors.' }
$processPolicy = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-InstallerProcessPolicy'
}, $false)
if (-not $processPolicy) { throw 'Shared MSI transport policy is missing.' }
. ([ScriptBlock]::Create($processPolicy.Extent.Text))
$sourceRoot = Split-Path -Parent $PSScriptRoot
$workflowSource = [IO.File]::ReadAllText((Join-Path $sourceRoot '.github/workflows/release.yml'))
$processSource = [IO.File]::ReadAllText((Join-Path $sourceRoot 'tools/msi_process.psm1'))
Test-InstallerProcessPolicy -ReleaseAction $workflowSource -MsiProcessSource $processSource
foreach ($mutation in @(
        @('module', '[int]$TimeoutSeconds = 300', '[int]$TimeoutSeconds = 0'),
        @('module', 'WaitForExit($TimeoutSeconds * 1000)', 'WaitForExit()'),
        @('module', '.ArgumentList.Add($argument)', '.Arguments = $argument'),
        @('workflow', 'Import-Module ./tools/msi_process.psm1 -Force', '# removed import'),
        @('workflow', 'tools/test_installer_lifecycle.ps1 -Full -DisposableMachine', '# removed matrix'),
        @('workflow', 'Assert-FlitzZipMsiPackageVersion -PackageVersion', 'UnreviewedVersion -PackageVersion')
    )) {
    $changedWorkflow = $workflowSource
    $changedProcess = $processSource
    if ($mutation[0] -eq 'module') {
        if (-not $processSource.Contains($mutation[1])) { throw 'MSI transport control did not find its source.' }
        $changedProcess = $processSource.Replace($mutation[1], $mutation[2])
    } else {
        if (-not $workflowSource.Contains($mutation[1])) { throw 'MSI workflow control did not find its source.' }
        $changedWorkflow = $workflowSource.Replace($mutation[1], $mutation[2])
    }
    $rejected = $false
    try { Test-InstallerProcessPolicy -ReleaseAction $changedWorkflow -MsiProcessSource $changedProcess }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'MSI policy accepted a missing native transport or qualification guard.' }
}
$function = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Test-ExternalComparisonNamePolicy'
}, $false)
if ($null -eq $function) { throw 'Production comparison policy is missing.' }
$policy = [ScriptBlock]::Create($function.Extent.Text + [Environment]::NewLine + 'Test-ExternalComparisonNamePolicy')

$dotnetFunction = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Assert-ExactDotnetSdkVersion'
}, $false)
if ($null -eq $dotnetFunction) { throw 'Exact .NET SDK workflow policy is missing.' }
$dotnetPolicy = [ScriptBlock]::Create(
    $dotnetFunction.Extent.Text + [Environment]::NewLine +
    'Assert-ExactDotnetSdkVersion -Line $candidate -WorkflowPath fixture.yml -LineNumber 1')
foreach ($candidate in @('  dotnet-version: "10.0.402"', 'dotnet-version: 8.0.426 # released', 'steps: []')) {
    & $dotnetPolicy
}
foreach ($candidate in @('dotnet-version: "10.0.x"', 'dotnet-version: "10.0"', 'dotnet-version: ${{ env.SDK }}')) {
    $rejected = $false
    try { & $dotnetPolicy }
    catch {
        if ($_.Exception.Message -notlike '*exact published version*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "Floating .NET SDK selector was accepted: $candidate" }
}

# Purpose: Reject traversal outside the production policy's source boundary before filesystem enumeration.
# Inputs: The production function's real enumeration call. Outputs: Actual source files or an explicit boundary failure.
$enumerationGuard = {
    [CmdletBinding()]
    param([string]$Path, [string]$LiteralPath, [switch]$Recurse, [switch]$File, [switch]$Force)
    $selected = if ($LiteralPath) { $LiteralPath } else { $Path }
    if ($selected -notin @((Join-Path $repo 'src'), (Join-Path $repo 'include'))) {
        throw 'Policy attempted to enumerate outside its source boundary.'
    }
    Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters
}
# Inject the guard only into this isolated invocation; never replace a host cmdlet.
$fixtureFunctions = @{'Get-ChildItem' = $enumerationGuard}

try {
    if (Test-Path -LiteralPath $repo) { throw 'Test fixture already exists.' }
    foreach ($directory in @('src/nested', 'include', 'out')) {
        [IO.Directory]::CreateDirectory((Join-Path $repo $directory)) | Out-Null
    }
    [IO.File]::WriteAllText((Join-Path $repo 'src/nested/clean.cpp'), 'ordinary product text')
    [IO.File]::WriteAllText((Join-Path $repo 'out/report.txt'), ('ban' + 'dizip'))
    $policy.InvokeWithContext($fixtureFunctions, $null)
    foreach ($name in @('src/nested/.private.cpp', 'include/fixture.hpp')) {
        $path = Join-Path $repo $name
        [IO.File]::WriteAllText($path, ('ban' + 'dizip'))
        $rejected = $false
        try { $policy.InvokeWithContext($fixtureFunctions, $null) }
        catch {
            if ($_.Exception.Message -notlike '*Competitor branding belongs in benchmark research*') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "Source policy missed $name" }
        [IO.File]::WriteAllText($path, 'ordinary product text')
    }
    $policy.InvokeWithContext($fixtureFunctions, $null)
    $hygieneAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'verify_change_hygiene.ps1'), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Changed-file hygiene contains PowerShell syntax errors.' }
    $binaryFunction = $hygieneAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Test-ChangedBinaryPolicy'
    }, $false)
    if ($null -eq $binaryFunction) { throw 'Production binary admission is missing.' }
    $binaryPolicy = [ScriptBlock]::Create(
        $binaryFunction.Extent.Text + [Environment]::NewLine + 'Test-ChangedBinaryPolicy -Path $candidate')
    foreach ($candidate in @('src/built.whl', 'third_party/upstream/fixture/built.WHL')) {
        $path = Join-Path $repo $candidate
        [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
        [IO.File]::WriteAllText($path, 'inert built-package fixture')
        $rejected = $false
        try { & $binaryPolicy }
        catch {
            if ($_.Exception.Message -notlike '*Generated binary artifact is not allowed*') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "Binary admission missed $candidate" }
        Remove-Item -LiteralPath $path
        & $binaryPolicy
    }
    $candidate = 'third_party/upstream/fixture/source.tar.gz'
    [IO.File]::WriteAllText((Join-Path $repo $candidate), 'inert source-archive fixture')
    & $binaryPolicy
    Write-Output 'Policy scan traversal and source-detection controls passed.'
} finally {
    if (Test-Path -LiteralPath $repo) {
        $resolved = [IO.Path]::GetFullPath((Get-Item -LiteralPath $repo).FullName)
        $prefix = $temporaryRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar + 'flitzzip-policy-scan-'
        if ($resolved -cne $repo -or -not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing cleanup outside the exact test fixture.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
