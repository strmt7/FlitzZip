param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = "Stop"

# Purpose: Convert a repository-relative path to a full path under the repository.
# Inputs: RepoRelativePath is the manifest path to resolve.
# Outputs: Returns an absolute path or throws when the path would escape the repository.
function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$RepoRelativePath)

    $root = [IO.Path]::GetFullPath($RepoRoot)
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $RepoRelativePath))
    if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes repository root: $RepoRelativePath"
    }
    return $candidate
}

# Purpose: Read text from a manifest file source.
# Inputs: Entry is a JSON notice with a source property.
# Outputs: Returns exact UTF-8 text from the source-controlled file.
function Read-NoticeSourceFile {
    param([Parameter(Mandatory = $true)]$Entry)

    $path = Resolve-RepoPath -RepoRelativePath ([string]$Entry.source)
    if (-not (Test-Path -LiteralPath $path)) {
        throw "License notice source does not exist: $($Entry.source)"
    }
    return [IO.File]::ReadAllText($path, [Text.UTF8Encoding]::new($false))
}

# Purpose: Select a short C++ raw-string delimiter that does not appear in text.
# Inputs: Prefix describes the string kind and Text is the raw literal body.
# Outputs: Returns a delimiter within the C++ 16-character raw-delimiter limit.
function Get-RawStringDelimiter {
    param(
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $safePrefix = $Prefix -replace "[^A-Za-z0-9_]", ""
    if ($safePrefix.Length -gt 8) {
        $safePrefix = $safePrefix.Substring(0, 8)
    }
    for ($index = 0; $index -lt 1000; ++$index) {
        $delimiter = "SZ$safePrefix$index"
        if (-not $Text.Contains(")$delimiter`"")) {
            return $delimiter
        }
    }
    throw "Unable to find a safe raw-string delimiter."
}

# Purpose: Create one or more adjacent C++ UTF-8 raw string literals.
# Inputs: Prefix names the delimiter family and Text is the exact text to embed.
# Outputs: Returns C++ string-literal tokens preserving content while avoiding compiler literal-size limits.
function Format-RawStringLiteral {
    param(
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $normalized = $Text -replace "`r`n", "`n"
    $maxChunkLength = 8000
    if ($normalized.Length -eq 0) {
        $delimiter = Get-RawStringDelimiter -Prefix $Prefix -Text ""
        return "R`"$delimiter()$delimiter`""
    }

    $literals = New-Object System.Collections.Generic.List[string]
    for ($offset = 0; $offset -lt $normalized.Length; $offset += $maxChunkLength) {
        $length = [Math]::Min($maxChunkLength, $normalized.Length - $offset)
        $chunk = $normalized.Substring($offset, $length)
        $delimiter = Get-RawStringDelimiter -Prefix "$Prefix$($literals.Count)" -Text $chunk
        $literals.Add("R`"$delimiter($chunk)$delimiter`"")
    }
    return ($literals -join "`n")
}

$manifestFullPath = if ([IO.Path]::IsPathRooted($ManifestPath)) {
    [IO.Path]::GetFullPath($ManifestPath)
} else {
    Resolve-RepoPath -RepoRelativePath $ManifestPath
}
if (-not $manifestFullPath.StartsWith([IO.Path]::GetFullPath($RepoRoot), [StringComparison]::OrdinalIgnoreCase)) {
    throw "License notice manifest path escapes repository root: $ManifestPath"
}
$manifest = Get-Content -LiteralPath $manifestFullPath -Raw | ConvertFrom-Json
if (-not $manifest.notices -or @($manifest.notices).Count -eq 0) {
    throw "License notice manifest must contain at least one notice."
}

$titles = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::Ordinal)
$notices = New-Object System.Collections.Generic.List[object]
foreach ($entry in @($manifest.notices)) {
    $title = [string]$entry.title
    if ([string]::IsNullOrWhiteSpace($title)) {
        throw "License notice title must not be empty."
    }
    $group = [string]$entry.group
    if ($group -ne "SuperZip" -and $group -ne "Other") {
        throw "License notice '$title' must declare group SuperZip or Other."
    }
    if (-not $titles.Add($title)) {
        throw "Duplicate license notice title: $title"
    }
    if (-not ($entry.PSObject.Properties.Name -contains "source")) {
        throw "License notice '$title' must define a source-controlled text file."
    }
    if (($entry.PSObject.Properties.Name -contains "archive") -or
        ($entry.PSObject.Properties.Name -contains "entry") -or
        ($entry.PSObject.Properties.Name -contains "archive_sha256")) {
        throw "License notice '$title' must not require build-time archive extraction."
    }
    $text = Read-NoticeSourceFile -Entry $entry
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "License notice '$title' produced empty text."
    }
    $notices.Add([pscustomobject]@{ title = $title; group = $group; text = $text })
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("// Generated from resources/licenses/license-notices.json by tools/generate_license_notices_header.ps1.")
$lines.Add("// Do not edit this file directly; update the manifest or its source files.")
$lines.Add("#pragma once")
$lines.Add("")
$lines.Add("#include <array>")
$lines.Add("#include <string_view>")
$lines.Add("")
$lines.Add("namespace superzip::app {")
$lines.Add("")
$lines.Add("struct LicenseNotice {")
$lines.Add("    std::string_view title;")
$lines.Add("    std::string_view group;")
$lines.Add("    std::string_view text;")
$lines.Add("};")
$lines.Add("")
$lines.Add("inline constexpr std::array<LicenseNotice, $($notices.Count)> kLicenseNotices{{")
for ($index = 0; $index -lt $notices.Count; ++$index) {
    $notice = $notices[$index]
    $titleLiteral = Format-RawStringLiteral -Prefix "TITLE_$index" -Text $notice.title
    $groupLiteral = Format-RawStringLiteral -Prefix "GROUP_$index" -Text $notice.group
    $textLiteral = Format-RawStringLiteral -Prefix "TEXT_$index" -Text $notice.text
    $lines.Add("    LicenseNotice{$titleLiteral, $groupLiteral,")
    $lines.Add("$textLiteral},")
}
$lines.Add("}};")
$lines.Add("")
$lines.Add("}  // namespace superzip::app")
$lines.Add("")
[IO.File]::WriteAllLines($OutputPath, $lines.ToArray(), [Text.UTF8Encoding]::new($false))
