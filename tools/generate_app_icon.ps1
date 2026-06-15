param(
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repo "resources\app\superzip.ico"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

Add-Type -AssemblyName System.Drawing

# Purpose: Render the SuperZip stacked logo into a transparent bitmap.
# Inputs: `Size` is the square bitmap side length in pixels.
# Outputs: Returns a disposable `System.Drawing.Bitmap`.
function New-SuperZipLogoBitmap {
    param([int]$Size)

    $bitmap = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $penWidth = [Math]::Max(2.0, $Size / 15.0)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 255, 48, 58)), $penWidth
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    try {
        $cx = $Size / 2.0
        $diamondWidth = $Size * 0.56
        $diamondHeight = $Size * 0.28
        $top = $Size * 0.18
        $step = $Size * 0.205
        for ($i = 0; $i -lt 3; ++$i) {
            $y = $top + ($i * $step)
            $points = @(
                [System.Drawing.PointF]::new($cx, $y),
                [System.Drawing.PointF]::new($cx + ($diamondWidth / 2.0), $y + ($diamondHeight / 2.0)),
                [System.Drawing.PointF]::new($cx, $y + $diamondHeight),
                [System.Drawing.PointF]::new($cx - ($diamondWidth / 2.0), $y + ($diamondHeight / 2.0))
            )
            $graphics.DrawPolygon($pen, $points)
        }
    } finally {
        $pen.Dispose()
        $graphics.Dispose()
    }
    return $bitmap
}

# Purpose: Convert a bitmap to PNG bytes for ICO embedding.
# Inputs: `Bitmap` is a disposable 32-bit ARGB bitmap.
# Outputs: Returns encoded PNG bytes.
function ConvertTo-PngBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $stream = New-Object System.IO.MemoryStream
    try {
        $Bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        return $stream.ToArray()
    } finally {
        $stream.Dispose()
    }
}

# Purpose: Write a multi-resolution Windows ICO file.
# Inputs: `Path` is the output file and `Images` contains width and PNG bytes.
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

$images = foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
    $bitmap = New-SuperZipLogoBitmap -Size $size
    try {
        [pscustomobject]@{
            Size = $size
            Bytes = ConvertTo-PngBytes -Bitmap $bitmap
        }
    } finally {
        $bitmap.Dispose()
    }
}

Write-IcoFile -Path $OutputPath -Images $images
Write-Host "Generated $OutputPath"
