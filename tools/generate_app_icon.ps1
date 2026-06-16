param(
    [string]$OutputPath = "",
    [string]$SvgPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repo "resources\app\superzip.ico"
}
if ([string]::IsNullOrWhiteSpace($SvgPath)) {
    $SvgPath = Join-Path $repo "resources\brand\superzip-logo.svg"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

Import-Module (Join-Path $PSScriptRoot "superzip_brand_logo.psm1") -Force

# Purpose: Write a multi-resolution Windows ICO file.
# Inputs: `Path` is the output file and `Images` contains width and raw DIB bytes.
# Outputs: Writes a deterministic ICO file.
function Write-IcoFile {
    param(
        [string]$Path,
        [object[]]$Images
    )

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $writer = New-Object System.IO.BinaryWriter $stream
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$Images.Count)
        $offset = 6 + (16 * $Images.Count)
        foreach ($image in $Images) {
            $encodedSize = if ($image.Size -eq 256) { 0 } else { $image.Size }
            $writer.Write([byte]$encodedSize)
            $writer.Write([byte]$encodedSize)
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$image.Bytes.Length)
            $writer.Write([uint32]$offset)
            $offset += $image.Bytes.Length
        }
        foreach ($image in $Images) {
            $writer.Write([byte[]]$image.Bytes)
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

$mark = Read-SuperZipLogoMark -SvgPath $SvgPath
$images = foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
    [pscustomobject]@{
        Size = $size
        Bytes = New-SuperZipLogoIcoImageBytes -Mark $mark -Size $size
    }
}

Write-IcoFile -Path $OutputPath -Images $images
Write-Host "Generated $OutputPath"
