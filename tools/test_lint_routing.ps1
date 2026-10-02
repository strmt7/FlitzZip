$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $repo 'CMakeLists.txt') -PathType Leaf)) {
    throw 'Lint routing requires the repository root.'
}
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'lint.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Lint script did not parse.' }

# Exercise trusted production helpers without executing the linter entry point.
foreach ($definition in $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
$assignment = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
    $node.Left.VariablePath.UserPath -eq 'cmakeFiles'
}, $true)
if ($null -eq $assignment) { throw 'CMake lint routing was not found.' }
$call = $assignment.Right.Find({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq 'Get-LintTargetFile'
}, $true)

# Purpose: Read one literal routing argument from the actual production call.
# Inputs: Name identifies an existing parameter; call is the trusted parsed CMake selector.
# Outputs: Returns its literal value, or fails instead of inventing a duplicate routing policy.
function Get-LintRoutingLiteral {
    param([Parameter(Mandatory = $true)][string]$Name)
    $elements = $call.CommandElements
    for ($index = 0; $index -lt $elements.Count - 1; ++$index) {
        if ($elements[$index] -is [Management.Automation.Language.CommandParameterAst] -and
            $elements[$index].ParameterName -eq $Name) {
            return $elements[$index + 1].SafeGetValue()
        }
    }
    throw "Missing literal lint-routing argument: $Name"
}

$filePattern = @(Get-LintRoutingLiteral -Name 'FilePattern')
$allPathspec = @(Get-LintRoutingLiteral -Name 'AllPathspec')
$expected = @('CMakeLists.txt', 'cmake/WriteRuntimeIdentity.cmake',
    'tests/cmake/test_zstd_legacy_patch.cmake', 'tools/fixture.cmake',
    'tests/fixture/CMakeLists.txt')
$excluded = @('third_party/vendor/CMakeLists.txt', 'third_party/vendor/file.cmake', 'docs/guide.md')
$selected = @(Get-LintTargetFile -ChangedPath ($expected + $excluded) `
    -AllPathspec $allPathspec -FilePattern $filePattern)
if (($selected -join '|') -ne (($expected | Sort-Object -Unique) -join '|')) {
    throw 'Changed CMake routing omitted owned files or included upstream/other files.'
}

$newPath = 'tests/cmake/not-yet-tracked.cmake'
$all = @(Get-LintTargetFile -ChangedPath @($newPath, $excluded[1]) `
    -AllPathspec $allPathspec -FilePattern $filePattern -ForceAll)
if ($all -notcontains $newPath -or $all -notcontains 'CMakeLists.txt' -or
    $all -contains $excluded[1] -or $all.Count -ne @($all | Sort-Object -Unique).Count) {
    throw 'Expanded CMake routing lost a new file, included upstream code or duplicated targets.'
}
$configuration = @(Get-LintTargetFile -ChangedPath @('.ruff.toml', 'tools/new_fixture.py') `
    -AllPathspec @('tools/*.py') -FilePattern @('^tools/.*\.py$') `
    -ConfigPattern @('^\.ruff\.toml$'))
if ($configuration -notcontains 'tools/new_fixture.py' -or
    $configuration -notcontains 'tools/test_benchmark_graph.py') {
    throw 'A configuration-triggered whole-language pass lost a new source file.'
}
Write-Output 'Lint routing changed/all/configuration/new-file checks passed.'
