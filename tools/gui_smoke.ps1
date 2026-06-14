param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$ScreenshotPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Configuration\SuperZip.exe"
if (-not (Test-Path $exe)) {
    throw "SuperZip.exe not found. Run tools/build.ps1 first."
}
if ([string]::IsNullOrWhiteSpace($ScreenshotPath)) {
    $ScreenshotPath = Join-Path $repo "out\gui-smoke-$Configuration.png"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ScreenshotPath) | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class SuperZipNativeUi {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, int flags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hWnd);
}
"@

# Purpose: Build a Win32 mouse-coordinate LPARAM.
# Inputs: `X` and `Y` are client-area physical pixel coordinates.
# Outputs: Returns the LPARAM expected by mouse messages.
function New-MouseLParam {
    param(
        [int]$X,
        [int]$Y
    )
    return [IntPtr]((($Y -band 0xffff) -shl 16) -bor ($X -band 0xffff))
}

# Purpose: Click one sidebar navigation item in the SuperZip window.
# Inputs: `Handle` is the SuperZip HWND, `Dpi` is the window DPI, and `PageIndex` is zero-based.
# Outputs: Posts left-button mouse messages and lets the app route the page change.
function Invoke-SidebarClick {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [int]$PageIndex
    )
    $scale = [double]$Dpi / 96.0
    $x = [int][Math]::Round(43 * $scale)
    $y = [int][Math]::Round((93 + ($PageIndex * 63)) * $scale)
    $lparam = New-MouseLParam -X $x -Y $y
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0201, [IntPtr]1, $lparam)
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0202, [IntPtr]::Zero, $lparam)
}

# Purpose: Capture and validate the current SuperZip window pixels.
# Inputs: `Handle` is the SuperZip HWND and `Path` is the PNG output path.
# Outputs: Writes a screenshot and returns width, height, and sampled-color metadata.
function Save-SuperZipScreenshot {
    param(
        [IntPtr]$Handle,
        [string]$Path
    )
    [SuperZipNativeUi+RECT]$rect = New-Object SuperZipNativeUi+RECT
    if (-not [SuperZipNativeUi]::GetWindowRect($Handle, [ref]$rect)) {
        throw "Could not read SuperZip window rectangle."
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 720 -or $height -lt 480) {
        throw "SuperZip window is unexpectedly small: ${width}x${height}."
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try {
        $rendered = [SuperZipNativeUi]::PrintWindow($Handle, $hdc, 2)
    } finally {
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
    }
    if (-not $rendered) {
        throw "PrintWindow failed for SuperZip."
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

    $unique = New-Object 'System.Collections.Generic.HashSet[int]'
    for ($x = 0; $x -lt $width; $x += [Math]::Max(1, [int]($width / 40))) {
        for ($y = 0; $y -lt $height; $y += [Math]::Max(1, [int]($height / 30))) {
            [void]$unique.Add($bitmap.GetPixel($x, $y).ToArgb())
        }
    }
    $bitmap.Dispose()
    if ($unique.Count -lt 8) {
        throw "SuperZip screenshot appears blank or visually invalid."
    }
    return [pscustomobject]@{
        Path = $Path
        Width = $width
        Height = $height
        UniqueColors = $unique.Count
    }
}

$previousDpiContext = [SuperZipNativeUi]::SetThreadDpiAwarenessContext([IntPtr](-4))
$process = Start-Process -FilePath $exe -PassThru
$windowHandle = [IntPtr]::Zero
try {
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            throw "SuperZip exited before showing a window. ExitCode=$($process.ExitCode)."
        }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($null -ne $process.MainWindowHandle -and $process.MainWindowHandle -ne 0) {
            $windowHandle = [IntPtr]$process.MainWindowHandle
            break
        }
    }
    if ($windowHandle -eq [IntPtr]::Zero) {
        throw "SuperZip window did not appear."
    }

    Start-Sleep -Seconds 2
    $windowDpi = [int][SuperZipNativeUi]::GetDpiForWindow($windowHandle)
    if ($windowDpi -le 0) {
        $windowDpi = 96
    }
    $captures = @()
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $ScreenshotPath
    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "AMD-GPU", "Preferences", "About")
    $basePath = Join-Path (Split-Path -Parent $ScreenshotPath) ([System.IO.Path]::GetFileNameWithoutExtension($ScreenshotPath))
    $extension = [System.IO.Path]::GetExtension($ScreenshotPath)
    for ($index = 0; $index -lt $pageNames.Count; ++$index) {
        Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex $index
        Start-Sleep -Milliseconds 300
        $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path "${basePath}-$($pageNames[$index])$extension"
    }
    $captures | ConvertTo-Json
} finally {
    if ($process -and -not $process.HasExited) {
        if ($windowHandle -ne [IntPtr]::Zero) {
            [void][SuperZipNativeUi]::PostMessage($windowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force
        }
    }
    if ($previousDpiContext -ne [IntPtr]::Zero) {
        [void][SuperZipNativeUi]::SetThreadDpiAwarenessContext($previousDpiContext)
    }
}
