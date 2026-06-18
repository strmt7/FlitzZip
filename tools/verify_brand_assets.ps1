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

# Purpose: Parse an SVG `translate(x y)` transform into numeric offsets.
# Inputs: `Transform` is the transform attribute from the canonical SVG mark group.
# Outputs: Returns X and Y translation values or throws on unsupported transforms.
function ConvertFrom-SvgTranslate {
    param([AllowEmptyString()][string]$Transform)

    if ($Transform -notmatch '^\s*translate\(\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))(?:[\s,]+([-+]?(?:\d+(?:\.\d*)?|\.\d+)))?\s*\)\s*$') {
        throw "Canonical SuperZip logo mark must use a simple translate(x y) transform."
    }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $x = [double]::Parse($Matches[1], $culture)
    $y = if ($Matches[2]) { [double]::Parse($Matches[2], $culture) } else { 0.0 }
    return [pscustomobject]@{ X = $x; Y = $y }
}

# Purpose: Verify the canonical SVG canvas fully contains the stroked mark.
# Inputs: `Document` is the parsed SVG and `Mark` is the canonical mark group.
# Outputs: Throws when GitHub or app consumers would clip the vector logo.
function Assert-LogoMarkNotClipped {
    param(
        [Parameter(Mandatory = $true)]$Document,
        [Parameter(Mandatory = $true)]$Mark
    )

    $viewBox = $Document.DocumentElement.GetAttribute("viewBox")
    if ($viewBox -notmatch '^\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s*$') {
        throw "resources\brand\superzip-logo.svg must define a numeric viewBox."
    }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $viewMinX = [double]::Parse($Matches[1], $culture)
    $viewMinY = [double]::Parse($Matches[2], $culture)
    $viewMaxX = $viewMinX + [double]::Parse($Matches[3], $culture)
    $viewMaxY = $viewMinY + [double]::Parse($Matches[4], $culture)
    $translation = ConvertFrom-SvgTranslate -Transform ($Mark.GetAttribute("transform"))
    $strokePadding = [double]::Parse($Mark.GetAttribute("stroke-width"), $culture) / 2.0
    $allNumbers = foreach ($path in @($Mark.ChildNodes | Where-Object { $_.LocalName -eq "path" })) {
        [regex]::Matches($path.GetAttribute("d"), "[-+]?(?:\d+(?:\.\d*)?|\.\d+)") | ForEach-Object {
            [double]::Parse($_.Value, $culture)
        }
    }
    $xValues = for ($i = 0; $i -lt $allNumbers.Count; $i += 2) { $allNumbers[$i] + $translation.X }
    $yValues = for ($i = 1; $i -lt $allNumbers.Count; $i += 2) { $allNumbers[$i] + $translation.Y }
    $minX = ($xValues | Measure-Object -Minimum).Minimum - $strokePadding
    $maxX = ($xValues | Measure-Object -Maximum).Maximum + $strokePadding
    $minY = ($yValues | Measure-Object -Minimum).Minimum - $strokePadding
    $maxY = ($yValues | Measure-Object -Maximum).Maximum + $strokePadding
    if ($minX -lt $viewMinX -or $maxX -gt $viewMaxX -or $minY -lt $viewMinY -or $maxY -gt $viewMaxY) {
        throw "Canonical SuperZip SVG mark is clipped by its viewBox."
    }
}

# Purpose: Verify AI agents did not modify the canonical logo mark artwork.
# Inputs: `Mark` is the `superzip-logo-mark` group from the canonical SVG.
# Outputs: Throws when mark paths, stroke style, or source-of-truth metadata change.
function Assert-LogoMarkArtworkContract {
    param([Parameter(Mandatory = $true)]$Mark)

    $expectedPaths = @(
        "M84 0 L168 42 L84 84 L0 42 Z",
        "M84 62 L168 104 L84 146 L0 104 Z",
        "M84 124 L168 166 L84 208 L0 166 Z"
    )
    if ($Mark.GetAttribute("data-source-of-truth") -ne "true") {
        throw "Canonical SuperZip logo mark must keep data-source-of-truth=true."
    }
    if ($Mark.GetAttribute("fill") -ne "none" -or $Mark.GetAttribute("stroke") -ne "#ff303a") {
        throw "AI agents must not modify the canonical SuperZip logo mark fill or stroke color."
    }
    if ($Mark.GetAttribute("stroke-width") -ne "16") {
        throw "AI agents must not modify the canonical SuperZip logo mark stroke width."
    }
    if ($Mark.GetAttribute("stroke-linejoin") -ne "round" -or $Mark.GetAttribute("stroke-linecap") -ne "round") {
        throw "AI agents must not modify the canonical SuperZip logo mark stroke joins or caps."
    }
    $paths = @($Mark.ChildNodes | Where-Object { $_.LocalName -eq "path" })
    if ($paths.Count -ne $expectedPaths.Count) {
        throw "AI agents must not add or remove canonical SuperZip logo mark layers."
    }
    for ($i = 0; $i -lt $expectedPaths.Count; ++$i) {
        if ($paths[$i].GetAttribute("d") -ne $expectedPaths[$i]) {
            throw "AI agents must not modify canonical SuperZip logo mark path geometry."
        }
    }
}

# Purpose: Verify product wordmark text in the canonical SVG stays intentional.
# Inputs: `Document` is the parsed SVG source of truth.
# Outputs: Throws when the public README/app logo tagline regresses.
function Assert-LogoTextContract {
    param([Parameter(Mandatory = $true)]$Document)

    $textValues = @($Document.GetElementsByTagName("text") | ForEach-Object { $_.InnerText })
    if ($textValues -notcontains "SuperZip") {
        throw "Canonical SuperZip SVG must contain the SuperZip wordmark."
    }
    if ($textValues -notcontains "ULTRAFAST GPU-ACCELERATED ARCHIVAL SOFTWARE") {
        throw "Canonical SuperZip SVG must contain the approved ultrafast GPU-accelerated archival software tagline."
    }
}

try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    [xml]$document = Get-Content -LiteralPath $svg -Raw
    $mark = $document.GetElementsByTagName("g") | Where-Object { $_.GetAttribute("id") -eq "superzip-logo-mark" } | Select-Object -First 1
    if ($null -eq $mark -or $mark.GetAttribute("data-source-of-truth") -ne "true") {
        throw "resources\brand\superzip-logo.svg must contain the canonical superzip-logo-mark source-of-truth group."
    }
    Assert-LogoMarkArtworkContract -Mark $mark
    Assert-LogoTextContract -Document $document
    Assert-LogoMarkNotClipped -Document $document -Mark $mark

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
    $drawLogoCallCount = ([regex]::Matches($mainWindow, '\bdraw_logo\s*\(')).Count - 1
    if ($drawLogoCallCount -ne 2) {
        throw "The Win32 app must use exactly two canonical draw_logo call sites: top bar and About page."
    }
    $readme = Get-Content -LiteralPath (Join-Path $repo "README.md") -Raw
    if ($readme -notmatch '<img src="resources/brand/superzip-logo\.svg"') {
        throw "README must render the SuperZip logo from the canonical SVG source."
    }
    if ($mainWindow -notmatch 'ULTRAFAST GPU-ACCELERATED ARCHIVAL SOFTWARE') {
        throw "The Win32 About page must use the canonical SuperZip SVG tagline."
    }
    if ($mainWindow -match 'Native Windows AMD HIP archive utility') {
        throw "The Win32 About page must not use the retired product tagline."
    }

    Write-Output "Brand asset verification passed."
} finally {
    if (Test-Path -LiteralPath $work) {
        $resolved = (Resolve-Path -LiteralPath $work).Path
        $expectedPrefix = Join-Path $repo "out\brand-verify-"
        if ($resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
