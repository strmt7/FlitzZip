$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gui_settings_choices.ps1')

$tokens = $null
$parseErrors = $null
$helper = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'gui_settings_choices.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors) { throw 'GUI choice helper does not parse.' }
$defined = @($helper.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true) | ForEach-Object Name)
# Purpose: Detect unresolved semantic-choice calls in the actual helper and GUI harness without opening a window.
# Inputs: Source text and the helper's declared function names. Outputs: Rejects parse errors or missing bindings.
function Assert-GuiSettingsBinding {
    param([string]$Source, [string[]]$Defined)
    $parseTokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Source, [ref]$parseTokens, [ref]$errors)
    if ($errors) { throw 'GUI settings consumer does not parse.' }
    $calls = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -like 'Get-GuiSettingsChoice*'
    }, $true))
    if (-not $calls.Count) { throw 'GUI settings consumer no longer uses semantic choices.' }
    foreach ($call in $calls) {
        if ($call.GetCommandName() -notin $Defined) { throw "Unresolved GUI choice helper: $($call.GetCommandName())" }
    }
}
$consumerSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'gui_smoke.ps1'))
Assert-GuiSettingsBinding -Source $consumerSource -Defined $defined
Assert-GuiSettingsBinding -Source $helper.Extent.Text -Defined $defined
$rejectedBinding = $false
try {
    Assert-GuiSettingsBinding -Source $consumerSource.Replace('Get-GuiSettingsChoiceValue', 'Get-GuiSettingsChoiceMissing') -Defined $defined
} catch { $rejectedBinding = $_.Exception.Message -like 'Unresolved GUI choice helper:*' }
if (-not $rejectedBinding) { throw 'The actual GUI call-binding guard missed an unresolved helper control.' }

foreach ($kind in @('LogLevel', 'Retention', 'Format')) {
    $field = switch ($kind) { 'LogLevel' { 'logLevel' }; 'Retention' { 'logRetentionDays' }; 'Format' { 'compressionFormat' } }
    $values = @(Get-GuiSettingsChoiceValue -Kind $kind)
    if (@($values | Sort-Object -Unique).Count -ne $values.Count) { throw "Duplicate semantic IDs: $kind" }
    for ($index = 0; $index -lt $values.Count; ++$index) {
        $document = @{ $field = $values[$index] } | ConvertTo-Json | ConvertFrom-Json
        if ((Get-GuiSettingsChoiceIndex -Settings $document -Kind $kind) -ne $index) {
            throw "JSON semantic choice did not preserve the expected gesture row: $kind/$index"
        }
    }
    $rejected = $false
    try { Get-GuiSettingsChoiceIndex -Settings ([pscustomobject]@{ $field = 'unsupported' }) -Kind $kind }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Unsupported choice was silently treated as a default: $kind" }
}
Write-Output 'GUI settings semantic choice contracts passed.'
