# SuperZip GUI smoke Win32, input, capture, and visual helper functions.
$ErrorActionPreference = "Stop"

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
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

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
    public static extern IntPtr GlobalFree(IntPtr hMem);

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

    public static IntPtr CreateDropHandle(string[] paths, int x, int y) {
        string joined = string.Join("\0", paths) + "\0\0";
        byte[] pathBytes = System.Text.Encoding.Unicode.GetBytes(joined);
        int headerSize = Marshal.SizeOf(typeof(DROPFILES));
        ulong totalSize = checked((ulong)headerSize + (ulong)pathBytes.LongLength);
        IntPtr handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, new UIntPtr(totalSize));
        if (handle == IntPtr.Zero) {
            throw new InvalidOperationException("GlobalAlloc failed for HDROP payload.");
        }
        IntPtr memory = GlobalLock(handle);
        if (memory == IntPtr.Zero) {
            GlobalFree(handle);
            throw new InvalidOperationException("GlobalLock failed for HDROP payload.");
        }
        try {
            Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "pFiles"), headerSize);
            Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "x"), x);
            Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "y"), y);
            Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "fNC"), 0);
            Marshal.WriteInt32(memory, (int)Marshal.OffsetOf(typeof(DROPFILES), "fWide"), 1);
            Marshal.Copy(pathBytes, 0, IntPtr.Add(memory, headerSize), pathBytes.Length);
        } catch {
            GlobalUnlock(handle);
            GlobalFree(handle);
            throw;
        }
        GlobalUnlock(handle);
        return handle;
    }
}
"@

# Purpose: Put SuperZip in a stable visible state before PrintWindow capture.
# Inputs: `Handle` is the SuperZip HWND.
# Outputs: Moves only the SuperZip window to a visible capture origin and requests foreground/top ordering.
function Show-SuperZipForeground {
    param([IntPtr]$Handle)
    [void][SuperZipNativeUi]::ShowWindow($Handle, 5)
    $swpNoSize = 0x0001
    $swpShowWindow = 0x0040
    $flags = $swpNoSize -bor $swpShowWindow
    [void][SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr](-1), 8, 8, 0, 0, $flags)
    [void][SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr](-2), 8, 8, 0, 0, $flags)
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
function ConvertTo-MouseLParam {
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
    $lparam = ConvertTo-MouseLParam -X $x -Y $y
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
    $lparam = ConvertTo-MouseLParam -X $x -Y $y
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0201, [IntPtr]1, $lparam)
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0202, [IntPtr]::Zero, $lparam)
}

# Purpose: Send one keyboard activation to the SuperZip window.
# Inputs: `Handle` is the HWND and `VirtualKey` is a Win32 VK_* code.
# Outputs: Posts key-down and key-up messages for keyboard-accessibility smoke checks.
function Invoke-ClientKey {
    param(
        [IntPtr]$Handle,
        [int]$VirtualKey
    )
    [void][SuperZipNativeUi]::SendMessage($Handle, 0x0100, [IntPtr]$VirtualKey, [IntPtr]::Zero)
    [void][SuperZipNativeUi]::SendMessage($Handle, 0x0101, [IntPtr]$VirtualKey, [IntPtr]::Zero)
}

# Purpose: Drag one client-coordinate point to another in the SuperZip window.
# Inputs: `Handle` is the HWND, `Dpi` maps design pixels, and coordinates are 96-DPI client pixels.
# Outputs: Posts down/move/up mouse messages for resize and slider-style controls.
function Invoke-ClientDrag {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [int]$StartX,
        [int]$StartY,
        [int]$EndX,
        [int]$EndY
    )
    $scale = [double]$Dpi / 96.0
    $startClientX = [int][Math]::Round($StartX * $scale)
    $startClientY = [int][Math]::Round($StartY * $scale)
    $endClientX = [int][Math]::Round($EndX * $scale)
    $endClientY = [int][Math]::Round($EndY * $scale)
    $start = ConvertTo-MouseLParam -X $startClientX -Y $startClientY
    $end = ConvertTo-MouseLParam -X $endClientX -Y $endClientY
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0201, [IntPtr]1, $start)
    Start-Sleep -Milliseconds 60
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0200, [IntPtr]1, $end)
    Start-Sleep -Milliseconds 60
    [void][SuperZipNativeUi]::PostMessage($Handle, 0x0202, [IntPtr]::Zero, $end)
}

# Purpose: Inject a file-drop payload into the SuperZip window.
# Inputs: `Handle` is the SuperZip HWND and `Paths` are absolute file paths.
# Outputs: Posts `WM_DROPFILES`; the app owns and releases the allocated HDROP handle.
function Invoke-FileDrop {
    param(
        [IntPtr]$Handle,
        [string[]]$Paths,
        [int]$Dpi = 96,
        [int]$DesignX = 300,
        [int]$DesignY = 320
    )
    $scale = [double]$Dpi / 96.0
    $dropX = [int][Math]::Round($DesignX * $scale)
    $dropY = [int][Math]::Round($DesignY * $scale)
    $dropHandle = [SuperZipNativeUi]::CreateDropHandle($Paths, $dropX, $dropY)
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

# Purpose: Assert that a design-space rectangle contains a specific UI color family.
# Inputs: `Path` is a PNG screenshot, `Dpi` maps design pixels to physical pixels, and `Expected*` is an RGB target.
# Outputs: Throws when too few pixels are close to the requested color.
function Assert-DesignRectHasColor {
    param(
        [string]$Path,
        [int]$Dpi,
        [int]$Left,
        [int]$Top,
        [int]$Right,
        [int]$Bottom,
        [int]$ClientOffsetX = 0,
        [int]$ClientOffsetY = 0,
        [int]$ExpectedRed,
        [int]$ExpectedGreen,
        [int]$ExpectedBlue,
        [int]$Tolerance = 32,
        [int]$MinPixels = 8
    )
    $scale = [double]$Dpi / 96.0
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $leftPx = [Math]::Max(0, $ClientOffsetX + [int][Math]::Round($Left * $scale))
        $topPx = [Math]::Max(0, $ClientOffsetY + [int][Math]::Round($Top * $scale))
        $rightPx = [Math]::Min($bitmap.Width, $ClientOffsetX + [int][Math]::Round($Right * $scale))
        $bottomPx = [Math]::Min($bitmap.Height, $ClientOffsetY + [int][Math]::Round($Bottom * $scale))
        if ($rightPx -le $leftPx -or $bottomPx -le $topPx) {
            throw "Invalid visual-color rectangle in $Path."
        }
        $matchingPixels = 0
        for ($x = $leftPx; $x -lt $rightPx; $x++) {
            for ($y = $topPx; $y -lt $bottomPx; $y++) {
                $pixel = $bitmap.GetPixel($x, $y)
                $redMatch = [Math]::Abs([int]$pixel.R - $ExpectedRed) -le $Tolerance
                $greenMatch = [Math]::Abs([int]$pixel.G - $ExpectedGreen) -le $Tolerance
                $blueMatch = [Math]::Abs([int]$pixel.B - $ExpectedBlue) -le $Tolerance
                if ($redMatch -and $greenMatch -and $blueMatch) {
                    $matchingPixels++
                    if ($matchingPixels -ge $MinPixels) {
                        return
                    }
                }
            }
        }
        throw "Expected at least $MinPixels matching pixels in $Path, found $matchingPixels."
    } finally {
        $bitmap.Dispose()
    }
}

# Purpose: Assert that the Queue empty-state prompt is centered in the drop-zone panel.
# Inputs: `Path` is a Queue screenshot, `Dpi` maps design pixels, and client offsets account for window chrome.
# Outputs: Throws when the muted prompt pixels are absent or their bounding center drifts away from the panel center.
function Assert-QueueEmptyMessageCentered {
    param(
        [string]$Path,
        [int]$Dpi,
        [int]$ClientOffsetX = 0,
        [int]$ClientOffsetY = 0
    )
    $scale = [double]$Dpi / 96.0
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $leftPx = [Math]::Max(0, $ClientOffsetX + [int][Math]::Round(156 * $scale))
        $topPx = [Math]::Max(0, $ClientOffsetY + [int][Math]::Round(166 * $scale))
        $rightPx = [Math]::Min($bitmap.Width, $ClientOffsetX + [int][Math]::Round(1130 * $scale))
        $bottomPx = [Math]::Min($bitmap.Height, $ClientOffsetY + [int][Math]::Round(668 * $scale))
        if ($rightPx -le $leftPx -or $bottomPx -le $topPx) {
            throw "Invalid Queue empty-message rectangle in $Path."
        }

        $matchingPixels = 0
        $minX = $rightPx
        $minY = $bottomPx
        $maxX = $leftPx
        $maxY = $topPx
        for ($x = $leftPx; $x -lt $rightPx; $x++) {
            for ($y = $topPx; $y -lt $bottomPx; $y++) {
                $pixel = $bitmap.GetPixel($x, $y)
                if (Test-ColorNear -Color $pixel -Red 151 -Green 168 -Blue 174 -Tolerance 70) {
                    $matchingPixels++
                    $minX = [Math]::Min($minX, $x)
                    $minY = [Math]::Min($minY, $y)
                    $maxX = [Math]::Max($maxX, $x)
                    $maxY = [Math]::Max($maxY, $y)
                }
            }
        }

        if ($matchingPixels -lt 80) {
            throw "Queue empty-state prompt was not visibly rendered in $Path."
        }

        $actualCenterX = ($minX + $maxX) / 2.0
        $actualCenterY = ($minY + $maxY) / 2.0
        $expectedCenterX = $ClientOffsetX + (643 * $scale)
        $expectedCenterY = $ClientOffsetY + (417 * $scale)
        $maxDeltaX = [Math]::Max(18, [int][Math]::Round(42 * $scale))
        $maxDeltaY = [Math]::Max(10, [int][Math]::Round(20 * $scale))
        if ([Math]::Abs($actualCenterX - $expectedCenterX) -gt $maxDeltaX) {
            throw "Queue empty-state prompt is not horizontally centered in $Path."
        }
        if ([Math]::Abs($actualCenterY - $expectedCenterY) -gt $maxDeltaY) {
            throw "Queue empty-state prompt is not vertically centered in $Path."
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
function Get-ValidatedScreenWindowCapture {
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
        Show-SuperZipForeground -Handle $Handle
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
            Show-SuperZipForeground -Handle $Handle
            Start-Sleep -Milliseconds 120
            $bitmap = Get-ValidatedScreenWindowCapture -Handle $Handle -Rect $rect -Width $width -Height $height
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


Export-ModuleMember -Function Assert-DesignRectHasColor, Assert-DesignRectHasDetail, Assert-FixedWindowStyle, Assert-QueueEmptyMessageCentered, Assert-SuperZipVisibleForCapture, Assert-VisualStructure, ConvertTo-MouseLParam, Get-ClientCaptureOffset, Get-SampledUniqueColorCount, Get-ValidatedScreenWindowCapture, Invoke-ClientClick, Invoke-ClientDrag, Invoke-ClientKey, Invoke-DropdownExercise, Invoke-FileDrop, Invoke-SidebarClick, Request-SuperZipRedraw, Save-SuperZipScreenshot, Show-SuperZipForeground, Test-ColorNear
