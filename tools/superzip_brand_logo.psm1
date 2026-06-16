$ErrorActionPreference = "Stop"

if (-not ("SuperZip.Branding.LogoRasterizer" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Globalization;

namespace SuperZip.Branding
{
    public static class LogoRasterizer
    {
        // Purpose: Render one deterministic 32-bit BGRA ICO DIB image from canonical logo coordinates.
        // Inputs: mark dimensions, stroke width/color, flattened layer coordinates, layer counts, and square icon size.
        // Outputs: Returns a DIB byte array containing a BITMAPINFOHEADER, bottom-up pixels, and an empty AND mask.
        public static byte[] RenderIcoImage(
            double markWidth,
            double markHeight,
            double strokeWidth,
            byte red,
            byte green,
            byte blue,
            string coordinateText,
            int layerCount,
            int pointsPerLayer,
            int size)
        {
            double[] coordinates = ParseCoordinates(coordinateText);
            if (coordinates == null)
            {
                throw new ArgumentNullException("coordinates");
            }
            if (size <= 0 || layerCount <= 0 || pointsPerLayer <= 1)
            {
                throw new ArgumentOutOfRangeException("size", "Icon size and layer dimensions must be positive.");
            }
            if (coordinates.Length != layerCount * pointsPerLayer * 2)
            {
                throw new ArgumentException("Flattened coordinates do not match the declared logo shape.", "coordinates");
            }

            double scale = Math.Min(size / markWidth, size / markHeight) * 0.82;
            double originX = (size - (markWidth * scale)) / 2.0;
            double originY = (size - (markHeight * scale)) / 2.0;
            double strokeRadius = Math.Max(0.7, (strokeWidth * scale) / 2.0);
            double radiusSquared = strokeRadius * strokeRadius;
            const int subsamples = 4;
            const int subsampleTotal = subsamples * subsamples;

            double[] scaled = new double[coordinates.Length];
            for (int i = 0; i < coordinates.Length; i += 2)
            {
                scaled[i] = originX + (coordinates[i] * scale);
                scaled[i + 1] = originY + (coordinates[i + 1] * scale);
            }

            int maskStride = ((size + 31) / 32) * 4;
            int pixelBytes = size * size * 4;
            int outputBytes = 40 + pixelBytes + (maskStride * size);
            byte[] output = new byte[outputBytes];
            WriteUInt32(output, 0, 40);
            WriteUInt32(output, 4, (uint)size);
            WriteUInt32(output, 8, (uint)(size * 2));
            WriteUInt16(output, 12, 1);
            WriteUInt16(output, 14, 32);
            WriteUInt32(output, 16, 0);
            WriteUInt32(output, 20, (uint)pixelBytes);
            WriteUInt32(output, 24, 0);
            WriteUInt32(output, 28, 0);
            WriteUInt32(output, 32, 0);
            WriteUInt32(output, 36, 0);

            int offset = 40;
            for (int y = size - 1; y >= 0; --y)
            {
                for (int x = 0; x < size; ++x)
                {
                    int covered = 0;
                    for (int sy = 0; sy < subsamples; ++sy)
                    {
                        for (int sx = 0; sx < subsamples; ++sx)
                        {
                            double sampleX = x + ((sx + 0.5) / subsamples);
                            double sampleY = y + ((sy + 0.5) / subsamples);
                            if (IsCovered(sampleX, sampleY, scaled, layerCount, pointsPerLayer, radiusSquared))
                            {
                                ++covered;
                            }
                        }
                    }

                    output[offset++] = blue;
                    output[offset++] = green;
                    output[offset++] = red;
                    output[offset++] = (byte)Math.Round(255.0 * covered / subsampleTotal, MidpointRounding.AwayFromZero);
                }
            }

            return output;
        }

        // Purpose: Parse invariant-culture flattened logo coordinates supplied by the PowerShell SVG reader.
        // Inputs: coordinateText is a comma-delimited list of x/y doubles.
        // Outputs: Returns parsed coordinate values or throws on malformed input.
        private static double[] ParseCoordinates(string coordinateText)
        {
            if (String.IsNullOrWhiteSpace(coordinateText))
            {
                throw new ArgumentException("Flattened coordinates are empty.", "coordinateText");
            }

            string[] parts = coordinateText.Split(new[] { ',' }, StringSplitOptions.None);
            double[] coordinates = new double[parts.Length];
            for (int i = 0; i < parts.Length; ++i)
            {
                coordinates[i] = Double.Parse(parts[i], CultureInfo.InvariantCulture);
            }
            return coordinates;
        }

        // Purpose: Test whether one sample point touches any stroked canonical logo segment.
        // Inputs: Sample point, scaled coordinates, logo topology, and squared stroke radius.
        // Outputs: Returns true when the sample contributes to pixel coverage.
        private static bool IsCovered(
            double pointX,
            double pointY,
            double[] scaled,
            int layerCount,
            int pointsPerLayer,
            double radiusSquared)
        {
            for (int layer = 0; layer < layerCount; ++layer)
            {
                int layerOffset = layer * pointsPerLayer * 2;
                for (int point = 0; point < pointsPerLayer; ++point)
                {
                    int a = layerOffset + (point * 2);
                    int nextPoint = (point + 1) % pointsPerLayer;
                    int b = layerOffset + (nextPoint * 2);
                    if (PointSegmentDistanceSquared(pointX, pointY, scaled[a], scaled[a + 1], scaled[b], scaled[b + 1]) <= radiusSquared)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Purpose: Compute squared distance from a point to a finite line segment.
        // Inputs: Point and segment endpoint coordinates in icon pixel space.
        // Outputs: Returns squared Euclidean distance.
        private static double PointSegmentDistanceSquared(double px, double py, double ax, double ay, double bx, double by)
        {
            double dx = bx - ax;
            double dy = by - ay;
            double lengthSquared = (dx * dx) + (dy * dy);
            if (lengthSquared <= 0.000001)
            {
                double ox = px - ax;
                double oy = py - ay;
                return (ox * ox) + (oy * oy);
            }

            double t = ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
            t = Math.Max(0.0, Math.Min(1.0, t));
            double cx = ax + (t * dx);
            double cy = ay + (t * dy);
            double ex = px - cx;
            double ey = py - cy;
            return (ex * ex) + (ey * ey);
        }

        // Purpose: Write an unsigned 16-bit value in little-endian order.
        // Inputs: Output buffer, byte offset, and value.
        // Outputs: Mutates two bytes in the output buffer.
        private static void WriteUInt16(byte[] output, int offset, ushort value)
        {
            output[offset] = (byte)(value & 0xFF);
            output[offset + 1] = (byte)((value >> 8) & 0xFF);
        }

        // Purpose: Write an unsigned 32-bit value in little-endian order.
        // Inputs: Output buffer, byte offset, and value.
        // Outputs: Mutates four bytes in the output buffer.
        private static void WriteUInt32(byte[] output, int offset, uint value)
        {
            output[offset] = (byte)(value & 0xFF);
            output[offset + 1] = (byte)((value >> 8) & 0xFF);
            output[offset + 2] = (byte)((value >> 16) & 0xFF);
            output[offset + 3] = (byte)((value >> 24) & 0xFF);
        }
    }
}
"@
}

# Purpose: Parse an invariant-culture SVG numeric value.
# Inputs: `Value` is a number token read from the canonical SuperZip SVG.
# Outputs: Returns the token as a double or throws on malformed input.
function ConvertFrom-SvgNumber {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "SVG numeric token is empty."
    }
    return [double]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture)
}

# Purpose: Parse one closed four-point logo layer path from the canonical SVG.
# Inputs: `PathData` is an SVG path using the `M x y L x y L x y L x y Z` subset.
# Outputs: Returns four ordered point objects; throws when the path leaves the supported logo subset.
function ConvertFrom-SuperZipLogoPath {
    param([Parameter(Mandatory = $true)][string]$PathData)

    $numberMatches = [regex]::Matches($PathData, "[-+]?(?:\d+(?:\.\d*)?|\.\d+)")
    if ($numberMatches.Count -ne 8 -or $PathData -notmatch "^\s*M" -or $PathData -notmatch "Z\s*$") {
        throw "Unsupported SuperZip logo path. Keep resources/brand/superzip-logo.svg in the documented four-point path subset."
    }
    $points = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $numberMatches.Count; $i += 2) {
        $points.Add([pscustomobject]@{
            X = ConvertFrom-SvgNumber -Value ($numberMatches[$i].Value)
            Y = ConvertFrom-SvgNumber -Value ($numberMatches[$i + 1].Value)
        })
    }
    return $points.ToArray()
}

# Purpose: Read the canonical SuperZip stacked-logo mark geometry from the repo SVG.
# Inputs: `SvgPath` points to `resources/brand/superzip-logo.svg`.
# Outputs: Returns view-box dimensions, stroke width, color, and ordered layer points.
function Read-SuperZipLogoMark {
    param([Parameter(Mandatory = $true)][string]$SvgPath)

    [xml]$document = Get-Content -LiteralPath $SvgPath -Raw
    $group = $document.GetElementsByTagName("g") | Where-Object { $_.GetAttribute("id") -eq "superzip-logo-mark" } | Select-Object -First 1
    if ($null -eq $group) {
        throw "Canonical SuperZip logo mark group was not found in $SvgPath."
    }
    $paths = @($group.ChildNodes | Where-Object { $_.LocalName -eq "path" })
    if ($paths.Count -ne 3) {
        throw "Canonical SuperZip logo mark must contain exactly three path layers."
    }

    $layers = New-Object System.Collections.Generic.List[object]
    foreach ($path in $paths) {
        $layers.Add([pscustomobject]@{
            Points = @(ConvertFrom-SuperZipLogoPath -PathData ($path.GetAttribute("d")))
        })
    }
    $allPoints = foreach ($layer in $layers) { $layer.Points }
    $minX = ($allPoints | Measure-Object -Property X -Minimum).Minimum
    $maxX = ($allPoints | Measure-Object -Property X -Maximum).Maximum
    $minY = ($allPoints | Measure-Object -Property Y -Minimum).Minimum
    $maxY = ($allPoints | Measure-Object -Property Y -Maximum).Maximum

    return [pscustomobject]@{
        Width = [double]($maxX - $minX)
        Height = [double]($maxY - $minY)
        StrokeWidth = ConvertFrom-SvgNumber -Value ($group.GetAttribute("stroke-width"))
        StrokeColor = $group.GetAttribute("stroke")
        Layers = $layers
    }
}

# Purpose: Convert a hex RGB color from the canonical SVG into channel values.
# Inputs: `Color` is a `#RRGGBB` value.
# Outputs: Returns red, green, and blue byte channel values.
function ConvertFrom-SvgColor {
    param([Parameter(Mandatory = $true)][string]$Color)

    if ($Color -notmatch "^#([0-9a-fA-F]{6})$") {
        throw "Unsupported SVG color format: $Color"
    }
    return [pscustomobject]@{
        R = [byte][Convert]::ToInt32($Matches[1].Substring(0, 2), 16)
        G = [byte][Convert]::ToInt32($Matches[1].Substring(2, 2), 16)
        B = [byte][Convert]::ToInt32($Matches[1].Substring(4, 2), 16)
    }
}

# Purpose: Render the canonical SuperZip stacked-logo mark into raw ICO image bytes.
# Inputs: `Mark` is returned by `Read-SuperZipLogoMark`; `Size` is the square bitmap side length.
# Outputs: Returns deterministic 32-bit BGRA DIB bytes with an empty AND mask.
function New-SuperZipLogoIcoImageBytes {
    param(
        [Parameter(Mandatory = $true)]$Mark,
        [Parameter(Mandatory = $true)][int]$Size
    )

    $color = ConvertFrom-SvgColor -Color $Mark.StrokeColor
    $coordinates = New-Object System.Collections.Generic.List[string]
    $invariantCulture = [Globalization.CultureInfo]::InvariantCulture
    $pointsPerLayer = -1
    $layerCount = 0
    foreach ($layer in $Mark.Layers) {
        ++$layerCount
        $points = @($layer.Points)
        if ($pointsPerLayer -lt 0) {
            $pointsPerLayer = $points.Count
        } elseif ($pointsPerLayer -ne $points.Count) {
            throw "Canonical SuperZip logo layers must use a consistent point count."
        }
        foreach ($point in $points) {
            $coordinates.Add(([double]$point.X).ToString("R", $invariantCulture))
            $coordinates.Add(([double]$point.Y).ToString("R", $invariantCulture))
        }
    }
    $coordinateText = $coordinates -join ","
    return [SuperZip.Branding.LogoRasterizer]::RenderIcoImage(
        [double]$Mark.Width,
        [double]$Mark.Height,
        [double]$Mark.StrokeWidth,
        [byte]$color.R,
        [byte]$color.G,
        [byte]$color.B,
        [string]$coordinateText,
        $layerCount,
        [int]$pointsPerLayer,
        [int]$Size)
}

Export-ModuleMember -Function Read-SuperZipLogoMark, New-SuperZipLogoIcoImageBytes
