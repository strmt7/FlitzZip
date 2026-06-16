$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$svg = Join-Path $repo "resources\brand\superzip-logo.svg"
$icon = Join-Path $repo "resources\app\superzip.ico"
$work = Join-Path $repo ("out\brand-verify-" + [guid]::NewGuid().ToString("N"))

# Purpose: Compare two files by SHA-256.
# Inputs: `Expected` and `Actual` are filesystem paths to compare.
# Outputs: Throws when hashes differ; otherwise returns normally.
function Assert-SameFileHash {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual
    )

    $expectedHash = (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash
    if ($expectedHash -ne $actualHash) {
        throw "Brand asset is stale. Regenerate with tools\generate_app_icon.ps1. Expected $Expected, got generated $Actual."
    }
}

try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    [xml]$document = Get-Content -LiteralPath $svg -Raw
    $mark = $document.GetElementsByTagName("g") | Where-Object { $_.GetAttribute("id") -eq "superzip-logo-mark" } | Select-Object -First 1
    if ($null -eq $mark -or $mark.GetAttribute("data-source-of-truth") -ne "true") {
        throw "resources\brand\superzip-logo.svg must contain the canonical superzip-logo-mark source-of-truth group."
    }

    $generatedHeader = Join-Path $work "superzip_brand_logo.hpp"
    & (Join-Path $repo "tools\generate_brand_logo_header.ps1") -SvgPath $svg -OutputPath $generatedHeader
    if (-not (Test-Path -LiteralPath $generatedHeader)) {
        throw "Brand logo header generation failed."
    }

    $generatedIcon = Join-Path $work "superzip.ico"
    & (Join-Path $repo "tools\generate_app_icon.ps1") -SvgPath $svg -OutputPath $generatedIcon
    if (-not (Test-Path -LiteralPath $generatedIcon)) {
        throw "Brand icon generation failed."
    }
    Assert-SameFileHash -Expected $icon -Actual $generatedIcon

    $cmakeLists = Get-Content -LiteralPath (Join-Path $repo "CMakeLists.txt") -Raw
    if ($cmakeLists -notmatch "generate_brand_logo_header\.ps1" -or $cmakeLists -notmatch "superzip_brand_logo\.hpp") {
        throw "CMake must generate the Win32 logo header from the canonical SVG."
    }
    $mainWindow = Get-Content -LiteralPath (Join-Path $repo "src\app\main_window.cpp") -Raw
    if ($mainWindow -notmatch '#include "superzip_brand_logo\.hpp"') {
        throw "The Win32 app must render the in-app mark from the generated SVG geometry header."
    }

    Write-Host "Brand asset verification passed."
} finally {
    if (Test-Path -LiteralPath $work) {
        $resolved = (Resolve-Path -LiteralPath $work).Path
        $expectedPrefix = Join-Path $repo "out\brand-verify-"
        if ($resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
