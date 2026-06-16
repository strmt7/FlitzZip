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
    const uint GMEM_MOVEABLE = 0x0002;
    const uint GMEM_ZEROINIT = 0x0040;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DROPFILES {
        public uint pFiles;
        public int x;
        public int y;
        [MarshalAs(UnmanagedType.Bool)]
        public bool fNC;
        [MarshalAs(UnmanagedType.Bool)]
        public bool fWide;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, int flags);

    [DllImport("user32.dll")]
    public static extern bool RedrawWindow(IntPtr hWnd, IntPtr lprcUpdate, IntPtr hrgnUpdate, uint flags);

    [DllImport("user32.dll")]
    public static extern bool UpdateWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll")]
    public static extern bool IsChild(IntPtr hWndParent, IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);

    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hWnd);

    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint="GetWindowLongW")]
    public static extern int GetWindowLongPtr32(IntPtr hWnd, int nIndex);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GlobalAlloc(uint uFlags, UIntPtr dwBytes);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GlobalLock(IntPtr hMem);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GlobalUnlock(IntPtr hMem);

    public static IntPtr GetWindowStyle(IntPtr hWnd) {
        if (IntPtr.Size == 8) {
            return GetWindowLongPtr64(hWnd, -16);
        }
        return new IntPtr(GetWindowLongPtr32(hWnd, -16));
    }

    public static IntPtr CreateDropHandle(string[] paths) {
        string joined = string.Join("\0", paths) + "\0\0";
        byte[] pathBytes = System.Text.Encoding.Unicode.GetBytes(joined);
        int headerSize = Marshal.SizeOf(typeof(DROPFILES));
        int totalSize = headerSize + pathBytes.Length;
        IntPtr handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, new UIntPtr((uint)totalSize));
        if (handle == IntPtr.Zero) {
            throw new InvalidOperationException("GlobalAlloc failed for HDROP payload.");
        }
        IntPtr memory = GlobalLock(handle);
        if (memory == IntPtr.Zero) {
            throw new InvalidOperationException("GlobalLock failed for HDROP payload.");
        }
        Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "pFiles"), headerSize);
        Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "x"), 0);
        Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "y"), 0);
        Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "fNC"), 0);
        Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "fWide"), 1);
        Marshal.Copy(pathBytes, 0, IntPtr.Add(memory, headerSize), pathBytes.Length);
        GlobalUnlock(handle);
        return handle;
    }
}
"@

# Purpose: Put SuperZip in a stable visible state before PrintWindow capture.
# Inputs: `Handle` is the SuperZip HWND.
# Outputs: Requests foreground/top ordering without creating or closing other windows.
function Set-SuperZipForeground {
    param([IntPtr]$Handle)
    [void][SuperZipNativeUi]::ShowWindow($Handle, 5)
    $swpNoSize = 0x0001
    $swpNoMove = 0x0002
    $swpShowWindow = 0x0040
    $flags = $swpNoSize -bor $swpNoMove -bor $swpShowWindow
    [void][SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr](-1), 0, 0, 0, 0, $flags)
    [void][SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr](-2), 0, 0, 0, 0, $flags)
    [void][SuperZipNativeUi]::BringWindowToTop($Handle)
    [void][SuperZipNativeUi]::SetForegroundWindow($Handle)
}

# Purpose: Assert that SuperZip owns visible sample points before screen fallback capture.
# Inputs: `Handle` is the SuperZip HWND and `Rect` is its screen rectangle.
# Outputs: Throws when another window covers sampled points inside the capture rectangle.
function Assert-SuperZipVisibleForCapture {
    param(
        [IntPtr]$Handle,
        [SuperZipNativeUi+RECT]$Rect
    )
    $points = @(
        @([int](($Rect.Left + $Rect.Right) / 2), [int](($Rect.Top + $Rect.Bottom) / 2)),
        @([int]($Rect.Left + (($Rect.Right - $Rect.Left) / 4)), [int]($Rect.Top + (($Rect.Bottom - $Rect.Top) / 4))),
        @([int]($Rect.Left + ((3 * ($Rect.Right - $Rect.Left)) / 4)), [int]($Rect.Top + ((3 * ($Rect.Bottom - $Rect.Top)) / 4)))
    )
    foreach ($pair in $points) {
        [SuperZipNativeUi+POINT]$point = New-Object SuperZipNativeUi+POINT
        $point.X = $pair[0]
        $point.Y = $pair[1]
        $owner = [SuperZipNativeUi]::WindowFromPoint($point)
        if ($owner -ne $Handle -and -not [SuperZipNativeUi]::IsChild($Handle, $owner)) {
            throw "Refusing GUI smoke screen fallback because another window covers SuperZip at $($point.X),$($point.Y): owner $owner."
        }
    }
}

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

# Purpose: Click one client-coordinate point in the SuperZip window.
# Inputs: `Handle` is the SuperZip HWND, `Dpi` is the window DPI, and design coordinates are 96-DPI client pixels.
# Outputs: Posts left-button mouse messages.
function Invoke-ClientClick {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [int]$DesignX,
        [int]$DesignY
    )
    $scale = [double]$Dpi / 96.0
    $x = [int][Math]::Round($DesignX * $scale)
    $y = [int][Math]::Round($DesignY * $scale)
    $lparam = New-MouseLParam -X $x -Y $y
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0201, [IntPtr]1, $lparam)
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0202, [IntPtr]::Zero, $lparam)
}

# Purpose: Inject a file-drop payload into the SuperZip window.
# Inputs: `Handle` is the SuperZip HWND and `Paths` are absolute file paths.
# Outputs: Posts `WM_DROPFILES`; the app owns and releases the allocated HDROP handle.
function Invoke-FileDrop {
    param(
        [IntPtr]$Handle,
        [string[]]$Paths
    )
    $dropHandle = [SuperZipNativeUi]::CreateDropHandle($Paths)
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0233, $dropHandle, [IntPtr]::Zero)
}

# Purpose: Assert that the release window is fixed-size.
# Inputs: `Handle` is the SuperZip HWND.
# Outputs: Throws when resize or maximize styles are present.
function Assert-FixedWindowStyle {
    param([IntPtr]$Handle)
    $style = [uint64][SuperZipNativeUi]::GetWindowStyle($Handle).ToInt64()
    $wsSizeBox = [uint64]0x00040000
    $wsMaximizeBox = [uint64]0x00010000
    if (($style -band $wsSizeBox) -ne 0 -or ($style -band $wsMaximizeBox) -ne 0) {
        throw ("SuperZip window is still resizable or maximizable. style=0x{0:x}" -f $style)
    }
}

# Purpose: Return the client area's origin inside the full-window screenshot.
# Inputs: `Handle` is the SuperZip HWND.
# Outputs: Returns `X` and `Y` offsets in physical pixels.
function Get-ClientCaptureOffset {
    param([IntPtr]$Handle)
    [SuperZipNativeUi+RECT]$window = New-Object SuperZipNativeUi+RECT
    if (-not [SuperZipNativeUi]::GetWindowRect($Handle, [ref]$window)) {
        throw "Could not read SuperZip window rectangle for client offset."
    }
    [SuperZipNativeUi+POINT]$point = New-Object SuperZipNativeUi+POINT
    $point.X = 0
    $point.Y = 0
    if (-not [SuperZipNativeUi]::ClientToScreen($Handle, [ref]$point)) {
        throw "Could not map SuperZip client origin to screen coordinates."
    }
    return [pscustomobject]@{
        X = $point.X - $window.Left
        Y = $point.Y - $window.Top
    }
}

# Purpose: Return whether a pixel is near an expected RGB color.
# Inputs: `Color` is the sampled pixel, RGB components define the target, and `Tolerance` is per-channel slack.
# Outputs: Returns true when all channels are within tolerance.
function Test-ColorNear {
    param(
        [System.Drawing.Color]$Color,
        [int]$Red,
        [int]$Green,
        [int]$Blue,
        [int]$Tolerance = 8
    )
    return ([Math]::Abs([int]$Color.R - $Red) -le $Tolerance) -and
        ([Math]::Abs([int]$Color.G - $Green) -le $Tolerance) -and
        ([Math]::Abs([int]$Color.B - $Blue) -le $Tolerance)
}

# Purpose: Assert that the fixed shell still has crisp, aligned structural bands.
# Inputs: `Bitmap` is a client-area screenshot and `Dpi` is the window DPI.
# Outputs: Throws when dimensions or primary separator lines drift.
function Assert-VisualStructure {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [int]$Dpi,
        [int]$ClientOffsetX,
        [int]$ClientOffsetY,
        [int]$ClientWidth,
        [int]$ClientHeight
    )
    $scale = [double]$Dpi / 96.0
    $expectedWidth = [int][Math]::Round(1200 * $scale)
    $expectedHeight = [int][Math]::Round(760 * $scale)
    if ([Math]::Abs($ClientWidth - $expectedWidth) -gt 2 -or [Math]::Abs($ClientHeight - $expectedHeight) -gt 2) {
        throw "Client dimensions drifted from the fixed design grid: ${ClientWidth}x${ClientHeight}, expected about ${expectedWidth}x${expectedHeight}."
    }

    $clientLeft = $ClientOffsetX
    $clientTop = $ClientOffsetY
    $clientRight = $ClientOffsetX + $ClientWidth
    $topSeparatorY = [Math]::Max(0, $clientTop + [int][Math]::Round(52 * $scale) - 1)
    $railSeparatorX = [Math]::Max(0, $clientLeft + [int][Math]::Round(86 * $scale) - 1)
    $statusSeparatorY = [Math]::Min($Bitmap.Height - 1, $clientTop + [int][Math]::Round((760 - 34) * $scale))
    $border = @{ Red = 54; Green = 72; Blue = 78 }

    $topMatches = 0
    $topTotal = 0
    for ($x = $clientLeft; $x -lt $clientRight; $x += [Math]::Max(1, [int]($ClientWidth / 60))) {
        ++$topTotal
        if (Test-ColorNear -Color $Bitmap.GetPixel($x, $topSeparatorY) -Red $border.Red -Green $border.Green -Blue $border.Blue) {
            ++$topMatches
        }
    }

    $railMatches = 0
    $railTotal = 0
    for ($y = $topSeparatorY + 1; $y -lt $statusSeparatorY; $y += [Math]::Max(1, [int](($statusSeparatorY - $topSeparatorY) / 40))) {
        ++$railTotal
        if (Test-ColorNear -Color $Bitmap.GetPixel($railSeparatorX, $y) -Red $border.Red -Green $border.Green -Blue $border.Blue) {
            ++$railMatches
        }
    }

    $statusMatches = 0
    $statusTotal = 0
    for ($x = $clientLeft; $x -lt $clientRight; $x += [Math]::Max(1, [int]($ClientWidth / 60))) {
        ++$statusTotal
        if (Test-ColorNear -Color $Bitmap.GetPixel($x, $statusSeparatorY) -Red $border.Red -Green $border.Green -Blue $border.Blue) {
            ++$statusMatches
        }
    }

    if (($topMatches / [double]$topTotal) -lt 0.75) {
        throw "Top command-bar separator is not visually continuous enough for the fixed design grid."
    }
    if (($railMatches / [double]$railTotal) -lt 0.75) {
        throw "Navigation rail separator is not visually continuous enough for the fixed design grid."
    }
    if (($statusMatches / [double]$statusTotal) -lt 0.75) {
        throw "Status-bar separator is not visually continuous enough for the fixed design grid."
    }
}

# Purpose: Assert that a design-space rectangle contains rendered detail.
# Inputs: `Path` is a PNG screenshot, `Dpi` maps design pixels to physical pixels, and the rectangle is in 96-DPI design coordinates.
# Outputs: Throws when the region is flat or blank.
function Assert-DesignRectHasDetail {
    param(
        [string]$Path,
        [int]$Dpi,
        [int]$Left,
        [int]$Top,
        [int]$Right,
        [int]$Bottom,
        [int]$ClientOffsetX = 0,
        [int]$ClientOffsetY = 0,
        [int]$MinUniqueColors = 5
    )
    $scale = [double]$Dpi / 96.0
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $leftPx = [Math]::Max(0, $ClientOffsetX + [int][Math]::Round($Left * $scale))
        $topPx = [Math]::Max(0, $ClientOffsetY + [int][Math]::Round($Top * $scale))
        $rightPx = [Math]::Min($bitmap.Width, $ClientOffsetX + [int][Math]::Round($Right * $scale))
        $bottomPx = [Math]::Min($bitmap.Height, $ClientOffsetY + [int][Math]::Round($Bottom * $scale))
        if ($rightPx -le $leftPx -or $bottomPx -le $topPx) {
            throw "Invalid visual-detail rectangle in $Path."
        }
        $unique = New-Object 'System.Collections.Generic.HashSet[int]'
        $stepX = [Math]::Max(1, [int](($rightPx - $leftPx) / 24))
        $stepY = [Math]::Max(1, [int](($bottomPx - $topPx) / 16))
        for ($x = $leftPx; $x -lt $rightPx; $x += $stepX) {
            for ($y = $topPx; $y -lt $bottomPx; $y += $stepY) {
                [void]$unique.Add($bitmap.GetPixel($x, $y).ToArgb())
            }
        }
        if ($unique.Count -lt $MinUniqueColors) {
            throw "Expected rendered dropdown/detail region in $Path, but sampled only $($unique.Count) unique colors."
        }
    } finally {
        $bitmap.Dispose()
    }
}

# Purpose: Force a pending SuperZip repaint before capturing visual assertions.
# Inputs: `Handle` is the SuperZip HWND to invalidate and update.
# Outputs: Requests synchronous client repaint; throws only if Win32 capture validation later fails.
function Request-SuperZipRedraw {
    param(
        [IntPtr]$Handle
    )
    $rdwInvalidate = 0x0001
    $rdwAllChildren = 0x0080
    $rdwUpdateNow = 0x0100
    [void][SuperZipNativeUi]::RedrawWindow($Handle, [IntPtr]::Zero, [IntPtr]::Zero, ($rdwInvalidate -bor $rdwAllChildren -bor $rdwUpdateNow))
    [void][SuperZipNativeUi]::UpdateWindow($Handle)
}

# Purpose: Count sampled unique colors in a captured bitmap.
# Inputs: `Bitmap` is the screenshot and `Width`/`Height` bound the sampled area.
# Outputs: Returns the sampled unique ARGB count.
function Get-SampledUniqueColorCount {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [int]$Width,
        [int]$Height
    )

    $unique = New-Object 'System.Collections.Generic.HashSet[int]'
    for ($x = 0; $x -lt $Width; $x += [Math]::Max(1, [int]($Width / 40))) {
        for ($y = 0; $y -lt $Height; $y += [Math]::Max(1, [int]($Height / 30))) {
            [void]$unique.Add($Bitmap.GetPixel($x, $y).ToArgb())
        }
    }
    return $unique.Count
}

# Purpose: Capture the visible SuperZip window rectangle after foreground validation.
# Inputs: `Rect` is the current window rectangle and `Width`/`Height` are physical pixel dimensions.
# Outputs: Returns a bitmap copied from the visible screen.
function New-ValidatedScreenWindowCapture {
    param(
        [IntPtr]$Handle,
        [SuperZipNativeUi+RECT]$Rect,
        [int]$Width,
        [int]$Height
    )

    Assert-SuperZipVisibleForCapture -Handle $Handle -Rect $Rect
    $bitmap = New-Object System.Drawing.Bitmap $Width, $Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($Rect.Left, $Rect.Top, 0, 0, [System.Drawing.Size]::new($Width, $Height))
    } finally {
        $graphics.Dispose()
    }
    return $bitmap
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

    $lastUniqueColors = 0
    for ($attempt = 1; $attempt -le 6; ++$attempt) {
        Set-SuperZipForeground -Handle $Handle
        Request-SuperZipRedraw -Handle $Handle
        Start-Sleep -Milliseconds (60 + ($attempt * 60))

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
            $bitmap.Dispose()
            if ($attempt -eq 6) {
                throw "PrintWindow failed for SuperZip."
            }
            continue
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

        $lastUniqueColors = Get-SampledUniqueColorCount -Bitmap $bitmap -Width $width -Height $height
        if ($lastUniqueColors -lt 8) {
            $bitmap.Dispose()
            Set-SuperZipForeground -Handle $Handle
            Start-Sleep -Milliseconds 120
            $bitmap = New-ValidatedScreenWindowCapture -Handle $Handle -Rect $rect -Width $width -Height $height
            $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
            $lastUniqueColors = Get-SampledUniqueColorCount -Bitmap $bitmap -Width $width -Height $height
            if ($lastUniqueColors -lt 8) {
                $bitmap.Dispose()
                continue
            }
        }

        try {
            [SuperZipNativeUi+RECT]$client = New-Object SuperZipNativeUi+RECT
            if (-not [SuperZipNativeUi]::GetClientRect($Handle, [ref]$client)) {
                throw "Could not read SuperZip client rectangle."
            }
            $offset = Get-ClientCaptureOffset -Handle $Handle
            Assert-VisualStructure -Bitmap $bitmap -Dpi ([int][SuperZipNativeUi]::GetDpiForWindow($Handle)) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ClientWidth ($client.Right - $client.Left) -ClientHeight ($client.Bottom - $client.Top)
            return [pscustomobject]@{
                Path = $Path
                Width = $width
                Height = $height
                UniqueColors = $lastUniqueColors
            }
        } finally {
            $bitmap.Dispose()
        }
    }
    throw "SuperZip screenshot appears blank or visually invalid after redraw attempts; last sampled $lastUniqueColors unique colors."
}

# Purpose: Open a dropdown, capture its expanded menu, verify it rendered, and select an option.
# Inputs: Coordinates are 96-DPI client design pixels; `BasePath`/`Extension` define the screenshot name.
# Outputs: Returns the dropdown screenshot capture metadata.
function Invoke-DropdownExercise {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [string]$Name,
        [int]$OpenX,
        [int]$OpenY,
        [int]$SelectX,
        [int]$SelectY,
        [int]$MenuLeft,
        [int]$MenuTop,
        [int]$MenuRight,
        [int]$MenuBottom,
        [string]$BasePath,
        [string]$Extension
    )
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $OpenX -DesignY $OpenY
    Start-Sleep -Milliseconds 180
    $path = "${BasePath}-Dropdown-$Name$Extension"
    $capture = Save-SuperZipScreenshot -Handle $Handle -Path $path
    $offset = Get-ClientCaptureOffset -Handle $Handle
    Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left $MenuLeft -Top $MenuTop -Right $MenuRight -Bottom $MenuBottom -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $SelectX -DesignY $SelectY
    Start-Sleep -Milliseconds 180
    return $capture
}

$smokeRoot = Join-Path $repo "out\gui-smoke-work"
New-Item -ItemType Directory -Force -Path $smokeRoot | Out-Null
$smokeInput = Join-Path $smokeRoot "drag-drop-input.txt"
$smokeFolder = Join-Path $smokeRoot "folder-input"
$smokeArchive = Join-Path $smokeRoot "valid-input.suzip"
$smokeCloseFile = Join-Path $smokeRoot "close.request"
Set-Content -LiteralPath $smokeInput -Value "SuperZip GUI smoke input" -NoNewline
New-Item -ItemType Directory -Force -Path $smokeFolder | Out-Null
Set-Content -LiteralPath (Join-Path $smokeFolder "nested.txt") -Value "Nested GUI smoke input" -NoNewline
Remove-Item -LiteralPath $smokeArchive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
& (Join-Path $repo "build\$Configuration\superzip_cli.exe") compress --format suzip --output $smokeArchive --force-cpu $smokeInput | Out-Null
if (-not (Test-Path -LiteralPath $smokeArchive)) {
    throw "Could not create valid SUZIP archive for GUI extract smoke."
}
$previousSmokeDestination = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", "Process")
$previousSmokeFiles = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", "Process")
$previousSmokeFolder = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", "Process")
$previousSmokeAutoClose = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", "Process")
$previousSmokeCloseFile = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_CLOSE_FILE", "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", $smokeRoot, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", (Resolve-Path -LiteralPath $smokeInput).Path, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", (Resolve-Path -LiteralPath $smokeFolder).Path, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", "90000", "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_CLOSE_FILE", $smokeCloseFile, "Process")

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
    Assert-FixedWindowStyle -Handle $windowHandle
    $captures = @()
    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "GPU", "Preferences", "About")
    $basePath = Join-Path (Split-Path -Parent $ScreenshotPath) ([System.IO.Path]::GetFileNameWithoutExtension($ScreenshotPath))
    $extension = [System.IO.Path]::GetExtension($ScreenshotPath)
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $ScreenshotPath

    # Queue header actions: exercise Add files, Add folder, and Clear without modal dialogs.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 918 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1032 -DesignY 91
    Start-Sleep -Milliseconds 150
    $pickerQueuePath = "${basePath}-Queue-AfterPickers$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $pickerQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $pickerQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150

    # Queue: exercise drag/drop and row selection only. Destination, level, and Start belong to Compress/Extract.
    Invoke-FileDrop -Handle $windowHandle -Paths @((Resolve-Path -LiteralPath $smokeInput).Path)
    Start-Sleep -Milliseconds 350
    $dropQueuePath = "${basePath}-Queue-AfterDragDrop$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $dropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $dropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 240 -DesignY 172
    Start-Sleep -Milliseconds 120

    # Compress: exercise fields, dropdowns, checkboxes, toggles, and Start.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 1
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Format" -OpenX 500 -OpenY 224 -SelectX 500 -SelectY 268 -MenuLeft 116 -MenuTop 252 -MenuRight 617 -MenuBottom 478 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Level" -OpenX 820 -OpenY 224 -SelectX 820 -SelectY 390 -MenuLeft 657 -MenuTop 252 -MenuRight 1158 -MenuBottom 414 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Method" -OpenX 500 -OpenY 294 -SelectX 500 -SelectY 370 -MenuLeft 116 -MenuTop 322 -MenuRight 617 -MenuBottom 388 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-BlockSize" -OpenX 820 -OpenY 294 -SelectX 820 -SelectY 426 -MenuLeft 657 -MenuTop 322 -MenuRight 1158 -MenuBottom 452 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 406
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 438
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 560 -DesignY 406
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 560 -DesignY 438
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 548
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 548
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 583
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 583
    Start-Sleep -Milliseconds 140
    $expectedTarGz = Join-Path $smokeRoot "SuperZip-output.tar.gz"
    Remove-Item -LiteralPath $expectedTarGz -Force -ErrorAction SilentlyContinue
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 500 -DesignY 224
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 500 -DesignY 364
    Start-Sleep -Milliseconds 160
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $createdTarGz = $false
    foreach ($attempt in 1..50) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $expectedTarGz) -and ((Get-Item -LiteralPath $expectedTarGz).Length -gt 0)) {
            $createdTarGz = $true
            break
        }
    }
    if (-not $createdTarGz) {
        throw "GUI compression did not create expected non-empty TAR.GZ archive at $expectedTarGz."
    }

    # Extract: return to Queue, clear inputs, drop a valid archive, then exercise extract controls.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-FileDrop -Handle $windowHandle -Paths @((Resolve-Path -LiteralPath $smokeArchive).Path)
    Start-Sleep -Milliseconds 250
    $archiveDropQueuePath = "${basePath}-Queue-ArchiveAfterDragDrop$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $archiveDropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $archiveDropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 560 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 520 -DesignY 227
    Start-Sleep -Milliseconds 120
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite" -OpenX 540 -OpenY 296 -SelectX 540 -SelectY 370 -MenuLeft 458 -MenuTop 326 -MenuRight 778 -MenuBottom 392 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 417
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 449
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 417
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 417
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 453
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 453
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Seconds 2

    # Security, History, GPU, and Preferences page controls.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 3
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Milliseconds 250

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 4
    Start-Sleep -Milliseconds 250
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Operation" -OpenX 220 -OpenY 145 -SelectX 220 -SelectY 246 -MenuLeft 116 -MenuTop 170 -MenuRight 336 -MenuBottom 300 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Status" -OpenX 390 -OpenY 145 -SelectX 390 -SelectY 214 -MenuLeft 354 -MenuTop 170 -MenuRight 574 -MenuBottom 268 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1100 -DesignY 90
    Start-Sleep -Milliseconds 250

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 5
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1100 -DesignY 92
    Start-Sleep -Milliseconds 250

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 6
    Start-Sleep -Milliseconds 250
    foreach ($point in @(
        @(175, 193),
        @(175, 227),
        @(175, 261),
        @(650, 193),
        @(175, 376),
        @(175, 412),
        @(175, 448),
        @(985, 666),
        @(1110, 666)
    )) {
        Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX $point[0] -DesignY $point[1]
        Start-Sleep -Milliseconds 140
    }
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Preferences-MemoryPolicy" -OpenX 700 -OpenY 247 -SelectX 700 -SelectY 318 -MenuLeft 622 -MenuTop 274 -MenuRight 887 -MenuBottom 372 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Preferences-LogLevel" -OpenX 700 -OpenY 384 -SelectX 700 -SelectY 456 -MenuLeft 622 -MenuTop 412 -MenuRight 887 -MenuBottom 510 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Preferences-LogRetention" -OpenX 700 -OpenY 442 -SelectX 700 -SelectY 514 -MenuLeft 622 -MenuTop 470 -MenuRight 887 -MenuBottom 568 -BasePath $basePath -Extension $extension

    for ($index = 0; $index -lt $pageNames.Count; ++$index) {
        Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex $index
        Start-Sleep -Milliseconds 300
        $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path "${basePath}-$($pageNames[$index])$extension"
    }
    $captures | ConvertTo-Json
} finally {
    $launchedProcessId = if ($process) { $process.Id } else { 0 }
    $cleanupFailure = $null
    Set-Content -LiteralPath $smokeCloseFile -Value "close" -NoNewline
    if ($process -and -not $process.HasExited) {
        if ($windowHandle -ne [IntPtr]::Zero) {
            [void][SuperZipNativeUi]::PostMessage($windowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        if (-not $process.WaitForExit(10000)) {
            try {
                Stop-Process -Id $process.Id -Force
            } catch {
                Write-Warning "Could not force-stop SuperZip process $($process.Id): $($_.Exception.Message)"
            }
        }
    }
    if ($launchedProcessId -ne 0 -and (Get-Process -Id $launchedProcessId -ErrorAction SilentlyContinue)) {
        $cleanupFailure = "SuperZip GUI smoke process $launchedProcessId did not exit cleanly."
    }
    if ($previousDpiContext -ne [IntPtr]::Zero) {
        [void][SuperZipNativeUi]::SetThreadDpiAwarenessContext($previousDpiContext)
    }
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", $previousSmokeDestination, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", $previousSmokeFiles, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", $previousSmokeFolder, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", $previousSmokeAutoClose, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_CLOSE_FILE", $previousSmokeCloseFile, "Process")
    Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
    if ($cleanupFailure) {
        throw $cleanupFailure
    }
}
