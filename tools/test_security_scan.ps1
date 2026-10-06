$ErrorActionPreference = 'Stop'
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$repo = Join-Path $temporaryRoot ('superzip-policy-scan-' + [Guid]::NewGuid().ToString('N'))
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'security_scan.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Policy scanner contains PowerShell syntax errors.' }
$function = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Test-ExternalComparisonNamePolicy'
}, $false)
if ($null -eq $function) { throw 'Production comparison policy is missing.' }
$policy = [ScriptBlock]::Create($function.Extent.Text + [Environment]::NewLine + 'Test-ExternalComparisonNamePolicy')

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
    Write-Output 'Policy scan traversal and source-detection controls passed.'
} finally {
    if (Test-Path -LiteralPath $repo) {
        $resolved = [IO.Path]::GetFullPath((Get-Item -LiteralPath $repo).FullName)
        $prefix = $temporaryRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar + 'superzip-policy-scan-'
        if ($resolved -cne $repo -or -not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing cleanup outside the exact test fixture.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
