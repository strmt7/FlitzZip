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
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

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
        DROPFILES drop = new DROPFILES {
            pFiles = (uint)headerSize,
            x = 0,
            y = 0,
            fNC = false,
            fWide = true
        };
        Marshal.StructureToPtr(drop, memory, false);
        Marshal.Copy(pathBytes, 0, IntPtr.Add(memory, headerSize), pathBytes.Length);
        GlobalUnlock(handle);
        return handle;
    }
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

$smokeRoot = Join-Path $repo "out\gui-smoke-work"
New-Item -ItemType Directory -Force -Path $smokeRoot | Out-Null
$smokeInput = Join-Path $smokeRoot "drag-drop-input.txt"
$smokeFolder = Join-Path $smokeRoot "folder-input"
$smokeArchive = Join-Path $smokeRoot "valid-input.suzip"
Set-Content -LiteralPath $smokeInput -Value "SuperZip GUI smoke input" -NoNewline
New-Item -ItemType Directory -Force -Path $smokeFolder | Out-Null
Set-Content -LiteralPath (Join-Path $smokeFolder "nested.txt") -Value "Nested GUI smoke input" -NoNewline
Remove-Item -LiteralPath $smokeArchive -Force -ErrorAction SilentlyContinue
& (Join-Path $repo "build\$Configuration\superzip_cli.exe") compress --format suzip --output $smokeArchive --force-cpu $smokeInput | Out-Null
if (-not (Test-Path -LiteralPath $smokeArchive)) {
    throw "Could not create valid SUZIP archive for GUI extract smoke."
}
$previousSmokeDestination = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", "Process")
$previousSmokeFiles = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", "Process")
$previousSmokeFolder = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", $smokeRoot, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", (Resolve-Path -LiteralPath $smokeInput).Path, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", (Resolve-Path -LiteralPath $smokeFolder).Path, "Process")

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
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $ScreenshotPath

    # Top command bar: exercise Add files, Add folder, and Clear without modal dialogs.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 256 -DesignY 25
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 366 -DesignY 25
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 464 -DesignY 25
    Start-Sleep -Milliseconds 150

    # Queue: exercise drag/drop, row selection, destination, profile, and Start.
    Invoke-FileDrop -Handle $windowHandle -Paths @((Resolve-Path -LiteralPath $smokeInput).Path)
    Start-Sleep -Milliseconds 350
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 240 -DesignY 172
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 280 -DesignY 640
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 180 -DesignY 690
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1085 -DesignY 648
    Start-Sleep -Seconds 2

    # Compress: exercise fields, dropdowns, checkboxes, toggles, and Start.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 1
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 224
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 500 -DesignY 294
    Start-Sleep -Milliseconds 120
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
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Seconds 2

    # Extract: clear queue, drop a valid archive, then exercise destination/options/toggles/Start.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 464 -DesignY 25
    Start-Sleep -Milliseconds 150
    Invoke-FileDrop -Handle $windowHandle -Paths @((Resolve-Path -LiteralPath $smokeArchive).Path)
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 520 -DesignY 227
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 540 -DesignY 296
    Start-Sleep -Milliseconds 120
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
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 220 -DesignY 145
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 390 -DesignY 145
    Start-Sleep -Milliseconds 120
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
        @(700, 265),
        @(175, 376),
        @(175, 412),
        @(175, 448),
        @(700, 384),
        @(700, 442),
        @(985, 666),
        @(1110, 666)
    )) {
        Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX $point[0] -DesignY $point[1]
        Start-Sleep -Milliseconds 140
    }

    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "GPU", "Preferences", "About")
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
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", $previousSmokeDestination, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", $previousSmokeFiles, "Process")
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", $previousSmokeFolder, "Process")
}
