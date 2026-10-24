param([Parameter(Mandatory = $true)][string]$SourcePath)

$ErrorActionPreference = 'Stop'
$parseErrors = $null
$tokens = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($SourcePath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'GUI driver does not parse.' }
foreach ($name in @('Get-GuiLiteralText', 'Assert-GuiBlockedText')) {
    $definition = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true)
    if ($null -eq $definition) { throw "Missing actual GUI helper: $name" }
    Set-Item -Path ("Function:" + $name) -Value $definition.Body.GetScriptBlock()
}
$required = 'L"1 week"; L"2 weeks"; L"1 month";'
$ordinary = @'
// Saved DC levels are not compression UI text.
/* L"levels" and L"Warnings" inside documentation are not labels. */
int levels = 2;
auto separator = 1'000;
auto quote = L'"';
auto text = L"escaped \"quote\" and // ordinary string content";
'@
Assert-GuiBlockedText -SourceText ($required + "`n" + $ordinary)
Assert-GuiBlockedText -SourceText ($required + "`n// continued \`n" + 'L"levels";')
$rejected = 0
foreach ($label in @('L"levels"', '"// levels"', 'u8R"tag(levels)tag"', 'L"Warnings"', 'L"Session"', 'L"Details"')) {
    $failed = $false
    try { Assert-GuiBlockedText -SourceText ($required + "`n" + $label) }
    catch { $failed = $true }
    if (-not $failed) { throw "Actual blocked literal was accepted: $label" }
    $rejected++
}
$failed = $false
try { Assert-GuiBlockedText -SourceText ('// ' + $required + "`n" + 'L"unrelated"') }
catch { $failed = $true }
if (-not $failed) { throw 'Comment-only required labels were accepted.' }
Write-Output "Actual GUI literal contracts passed; rejected=$rejected; comments, character literals, raw strings and identifiers checked."
