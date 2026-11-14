# Purpose: Run the canonical reviewed native cohort against the current successful build.
# Inputs: Base64 JSON changed paths and the configuration; resource containment belongs to the verifier.
# Outputs: Executes the selected cohort or explicitly defers measurement roles when CorrectnessOnly is requested.
param(
    [Parameter(Mandatory = $true)][string]$ChangedPathBase64,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$CpuOnlyValidation,
    [switch]$CorrectnessOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native_test_selection.ps1')
$root = Split-Path -Parent $PSScriptRoot
$paths = @(ConvertFrom-FlitzZipNativeSelectionPath -Base64 $ChangedPathBase64)
$selection = Get-FlitzZipNativeTestSelection -Paths $paths -Root $root
if ($selection.mode -ne 'component') { throw 'Changed inputs lack reviewed native component coverage; use the planned full native driver.' }
Invoke-FlitzZipNativeTestSelection -Selection $selection -TestRunner (Join-Path $root "build/$Configuration/flitzzip_tests.exe") -Cli (Join-Path $root "build/$Configuration/flitzzip_cli.exe") -CpuOnlyValidation:$CpuOnlyValidation -CorrectnessOnly:$CorrectnessOnly -Configuration $Configuration
