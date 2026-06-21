param()

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repo "resources\licenses\license-notices.json"
$generatorPath = Join-Path $repo "tools\generate_license_notices_header.ps1"

# Purpose: Assert that generated license notices are built from recorded source files only.
# Inputs: None; reads the license manifest and generator from the repository.
# Outputs: Throws on missing notice sources, stale generation failures, or GUI integration regressions.
function Test-LicenseNoticeGeneration {
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "License notice manifest is missing."
    }
    if (-not (Test-Path -LiteralPath $generatorPath)) {
        throw "License notice generator is missing."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $requiredTitles = @(
        "SuperZip",
        "miniz 3.1.1",
        "bzip2 1.0.8",
        "XZ Embedded",
        "LZMA SDK 26.01",
        "Zstandard 1.5.7",
        "Lhasa 0.5.0",
        "wimlib 1.14.5",
        "libdivsufsort-lite"
    )
    $titles = @($manifest.notices | ForEach-Object { [string]$_.title })
    foreach ($title in $requiredTitles) {
        if ($titles -notcontains $title) {
            throw "License notice manifest is missing required notice '$title'."
        }
    }
    $superZipNotice = @($manifest.notices | Where-Object { $_.title -eq "SuperZip" } | Select-Object -First 1)
    if (-not $superZipNotice -or [string]$superZipNotice.source -ne "LICENSE") {
        throw "The SuperZip license notice must be generated from the root LICENSE file."
    }
    if ([string]$superZipNotice.group -ne "SuperZip") {
        throw "The root SuperZip license must be the only notice in the SuperZip tab."
    }
    $misgrouped = @($manifest.notices | Where-Object {
            ($_.title -ne "SuperZip" -and [string]$_.group -ne "Other") -or
            ($_.title -eq "SuperZip" -and [string]$_.group -ne "SuperZip")
        })
    if ($misgrouped.Count -ne 0) {
        throw "Every non-root license notice must be grouped under the Other tab."
    }
    $archiveBacked = @($manifest.notices | Where-Object {
            ($_.PSObject.Properties.Name -contains "archive") -or
            ($_.PSObject.Properties.Name -contains "entry") -or
            ($_.PSObject.Properties.Name -contains "archive_sha256") -or
            -not ($_.PSObject.Properties.Name -contains "source")
        })
    if ($archiveBacked.Count -ne 0) {
        throw "License notices must use source-controlled text files, not build-time archive extraction."
    }
    $lzmaNotice = @($manifest.notices | Where-Object { $_.title -eq "LZMA SDK 26.01" } | Select-Object -First 1)
    if (-not $lzmaNotice -or [string]$lzmaNotice.source -ne "third_party/lzma_sdk/LICENSE") {
        throw "The LZMA SDK notice must use the checked-in upstream license text."
    }
    $tempHeader = Join-Path $env:TEMP ("superzip-license-notices-" + [guid]::NewGuid().ToString("N") + ".hpp")
    try {
        & $generatorPath -ManifestPath $manifestPath -RepoRoot $repo -OutputPath $tempHeader
        if (-not (Test-Path -LiteralPath $tempHeader)) {
            throw "License notice generator did not produce the expected header."
        }
        $generated = Get-Content -LiteralPath $tempHeader -Raw
        foreach ($title in $requiredTitles) {
            if (-not $generated.Contains($title)) {
                throw "Generated license notice header is missing '$title'."
            }
        }
    } finally {
        Remove-Item -LiteralPath $tempHeader -Force -ErrorAction SilentlyContinue
    }
}

# Purpose: Assert that the About page exposes generated notices through the SuperZip modal system.
# Inputs: None; scans GUI source contracts.
# Outputs: Throws when the GUI can drift to handwritten notices or platform-default dialogs.
function Test-LicenseNoticeGuiContract {
    $sources = @(
        (Join-Path $repo "src\app\main_window_license_dialog.cpp"),
        (Join-Path $repo "src\app\main_window_pages.cpp"),
        (Join-Path $repo "src\app\main_window_state.hpp"),
        (Join-Path $repo "src\app\main_window.hpp")
    )
    foreach ($source in $sources) {
        if (-not (Test-Path -LiteralPath $source)) {
            throw "License notice GUI source is missing: $source"
        }
    }
    $dialogSource = Get-Content -LiteralPath (Join-Path $repo "src\app\main_window_license_dialog.cpp") -Raw
    if ($dialogSource -notmatch '#include "superzip_license_notices\.hpp"') {
        throw "License dialog must include the generated notice header."
    }
    if ($dialogSource -match 'GNU GENERAL PUBLIC LICENSE|BSD License|ISC License|public domain by Igor Pavlov') {
        throw "License dialog must not hand-embed license text; use the generated notices."
    }
    if ($dialogSource -notmatch 'kLicenseNotices') {
        throw "License dialog must read from kLicenseNotices."
    }
    foreach ($requiredSnippet in @('license_notices_tab_rects', 'select_license_notices_tab', 'notice.group')) {
        if (-not $dialogSource.Contains($requiredSnippet)) {
            throw "License dialog must expose data-driven SuperZip/Other notice tabs; missing $requiredSnippet."
        }
    }
    if ($dialogSource -match 'CreateWindowExW.*(EDIT|RICHEDIT)|OpenClipboard|SetClipboardData|WM_COPY') {
        throw "License dialog must render non-selectable text and must not expose clipboard copy paths."
    }
    $aboutSource = Get-Content -LiteralPath (Join-Path $repo "src\app\main_window_pages.cpp") -Raw
    if (-not $aboutSource.Contains('L"Licenses"')) {
        throw "About page must expose a Licenses button."
    }
}

Test-LicenseNoticeGeneration
Test-LicenseNoticeGuiContract
Write-Output "License notice verification passed."
