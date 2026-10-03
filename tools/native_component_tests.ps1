# Purpose: Run the canonical reviewed native cohort against the current successful build.
# Inputs: Base64 JSON changed paths and the configuration; resource containment belongs to the verifier.
# Outputs: Executes only selected cases or fails on stale binaries, invalid paths or incomplete coverage.
param(
    [Parameter(Mandatory = $true)][string]$ChangedPathBase64,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native_test_selection.ps1')
$root = Split-Path -Parent $PSScriptRoot
$paths = @(ConvertFrom-SuperZipNativeSelectionPath -Base64 $ChangedPathBase64)
$selection = Get-SuperZipNativeTestSelection -Paths $paths -Root $root
if ($selection.mode -ne 'component') { throw 'Changed inputs lack reviewed native component coverage; use the planned full native driver.' }
$receiptArguments = @('-3', (Join-Path $PSScriptRoot 'native_build_receipt.py'), 'validate', '--root', $root, '--configuration', $Configuration)
if ($selection.requireHip) { $receiptArguments += '--require-hip' }
$receipt = & py @receiptArguments
if ($LASTEXITCODE -ne 0) { throw 'Native component tests require a current successful build receipt.' }
$receiptInfo = ($receipt -join [Environment]::NewLine) | ConvertFrom-Json
Write-Output "Native component receipt: $($receiptInfo.receipt_sha256)."
Invoke-SuperZipNativeTestSelection -Selection $selection -TestRunner (Join-Path $root "build/$Configuration/superzip_tests.exe") -Cli (Join-Path $root "build/$Configuration/superzip_cli.exe")
