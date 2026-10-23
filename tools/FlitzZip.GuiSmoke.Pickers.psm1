# FlitzZip real Windows common-dialog smoke. No selection-result overrides.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class FlitzZipPickerUi {
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string name);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr parameter, string text, uint flags, uint timeout, out UIntPtr result);
}
"@

# Purpose: Reject stale, foreign or unexpected HWNDs before sending native picker input.
# Inputs: Handle is the candidate; ProcessId is the exclusively owned GUI process; ClassName is the expected native class.
# Outputs: Throws on an invalid ownership or class boundary.
function Assert-PickerWindowOwner {
    param([IntPtr]$Handle, [int]$ProcessId, [string]$ClassName)
    [uint32]$owner = 0
    [void][FlitzZipPickerUi]::GetWindowThreadProcessId($Handle, [ref]$owner)
    $name = [Text.StringBuilder]::new(256)
    [void][FlitzZipPickerUi]::GetClassName($Handle, $name, $name.Capacity)
    if ($Handle -eq [IntPtr]::Zero -or $owner -ne $ProcessId -or $name.ToString() -cne $ClassName) {
        throw "Native picker ownership/class validation failed; expected owned $ClassName."
    }
}

# Purpose: Select or cancel real OS dialogs using owned controls rather than localized captions or injected results.
# Inputs: Handle/Dpi/ProcessId identify the owned GUI; Folder/Destination select the picker; Selection is an existing fixture path expression.
# Outputs: Returns only after the dialog closes and restores its owner, or throws after a bounded wait.
function Invoke-NativePickerSelection {
    param([IntPtr]$Handle, [int]$Dpi, [int]$ProcessId, [string]$Selection, [switch]$Folder, [switch]$Destination, [switch]$Cancel)
    Assert-PickerWindowOwner -Handle $Handle -ProcessId $ProcessId -ClassName 'FlitzZipMainWindow'
    $isFolder = $Folder -or $Destination
    $x = if ($Destination) { 820 } elseif ($Folder) { 1032 } else { 918 }
    $y = if ($Destination) { 154 } else { 91 }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $x -DesignY $y
    $deadline = [datetime]::UtcNow.AddSeconds(15)
    do {
        $popup = [FlitzZipPickerUi]::GetLastActivePopup($Handle)
        if ($popup -ne [IntPtr]::Zero -and $popup -ne $Handle -and -not [FlitzZipPickerUi]::IsWindowEnabled($Handle)) { break }
        Start-Sleep -Milliseconds 100
    } while ([datetime]::UtcNow -lt $deadline)
    Assert-PickerWindowOwner -Handle $popup -ProcessId $ProcessId -ClassName '#32770'
    # Folder mode exposes edt1 directly; multi-file mode exposes cmb13's embedded edit.
    $inputId = if ($isFolder) { 1152 } else { 1148 }
    $deadline = [datetime]::UtcNow.AddSeconds(10)
    do {
        $inputControl = [FlitzZipPickerUi]::GetDlgItem($popup, $inputId)
        if ($inputControl -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    } while ([datetime]::UtcNow -lt $deadline)
    $edit = $inputControl
    if (-not $isFolder) {
        Assert-PickerWindowOwner -Handle $inputControl -ProcessId $ProcessId -ClassName 'ComboBoxEx32'
        $inner = [FlitzZipPickerUi]::FindWindowEx($inputControl, [IntPtr]::Zero, 'ComboBox', $null)
        Assert-PickerWindowOwner -Handle $inner -ProcessId $ProcessId -ClassName 'ComboBox'
        $edit = [FlitzZipPickerUi]::FindWindowEx($inner, [IntPtr]::Zero, 'Edit', $null)
    }
    Assert-PickerWindowOwner -Handle $edit -ProcessId $ProcessId -ClassName 'Edit'
    if (-not [FlitzZipNativeUi]::PostMessage($popup, 0x0028, $edit, [IntPtr](1))) {
        throw 'Could not focus the native picker filename control.'
    }
    Start-Sleep -Milliseconds 100
    [UIntPtr]$result = [UIntPtr]::Zero
    if ([FlitzZipPickerUi]::SendMessageTimeout($edit, 0x000C, [IntPtr]::Zero, $Selection, 2, 2000, [ref]$result) -eq [IntPtr]::Zero -or $result -eq [UIntPtr]::Zero) {
        throw 'Native picker did not accept the fixture filename text.'
    }
    $buttonId = if ($Cancel) { 2 } else { 1 }
    $button = [FlitzZipPickerUi]::GetDlgItem($popup, $buttonId)
    Assert-PickerWindowOwner -Handle $button -ProcessId $ProcessId -ClassName 'Button'
    if (-not [FlitzZipNativeUi]::PostMessage($button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not queue native picker acceptance/cancellation.'
    }
    $deadline = [datetime]::UtcNow.AddSeconds(15)
    while ([FlitzZipPickerUi]::IsWindow($popup) -and [datetime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    if ([FlitzZipPickerUi]::IsWindow($popup) -or -not [FlitzZipPickerUi]::IsWindowEnabled($Handle)) {
        throw 'Native picker did not close and restore its owned main window.'
    }
    Start-Sleep -Milliseconds 300
}

# Purpose: Exercise destination acceptance and cancellation before independently checking archive publication.
# Inputs: Handle/Dpi/ProcessId identify the owned GUI; Destination/Cancelled are empty owned directories; BasePath/Extension locate captures.
# Outputs: Returns both Compress-page captures; the caller must verify the resulting archive location and cancelled directory.
function Assert-NativePickerDestination {
    param([IntPtr]$Handle, [int]$Dpi, [int]$ProcessId, [string]$Destination, [string]$Cancelled, [string]$BasePath, [string]$Extension)
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
    foreach ($cancel in @($false, $true)) {
        $scenario = if ($cancel) { 'Destination-Cancel' } else { 'Destination-Accept' }
        $selection = if ($cancel) { $Cancelled } else { $Destination }
        Write-Information "Real Windows picker scenario: $scenario" -InformationAction Continue
        Invoke-NativePickerSelection -Handle $Handle -Dpi $Dpi -ProcessId $ProcessId -Selection $selection -Destination -Cancel:$cancel
        Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-NativePicker-$scenario$Extension"
    }
}

# Purpose: Prove cancellation preserves all rendered Queue data, independently of product smoke-result hooks.
# Inputs: Before/After are owned-window captures; Handle/Dpi map the Queue body to physical pixels.
# Outputs: Throws on any changed Queue pixel; excludes the clock, toolbar hover and transient status strip.
function Assert-PickerQueueUnchanged {
    param([string]$Before, [string]$After, [IntPtr]$Handle, [int]$Dpi)
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $scale = $Dpi / 96.0
    $first = [Drawing.Bitmap]::new($Before)
    $second = [Drawing.Bitmap]::new($After)
    try {
        for ($y = [int]($offset.Y + 130 * $scale); $y -lt [int]($offset.Y + 310 * $scale); ++$y) {
            for ($x = [int]($offset.X + 120 * $scale); $x -lt [int]($offset.X + 1150 * $scale); ++$x) {
                if ($first.GetPixel($x, $y).ToArgb() -ne $second.GetPixel($x, $y).ToArgb()) {
                    throw 'Cancelling the real Windows picker changed the rendered Queue.'
                }
            }
        }
    } finally { $first.Dispose(); $second.Dispose() }
}

# Purpose: Exercise real input/destination picker acceptance and nonempty cancellation without selection overrides.
# Inputs: Exe is the qualified GUI; Root is owned smoke storage; BasePath/Extension locate captures; VerifyQueue independently reads the resulting archive.
# Outputs: Queue and archive captures with exact selection checks; always closes only this owned child process.
function Assert-NativePickerQueue {
    param([string]$Exe, [string]$Root, [string]$BasePath, [string]$Extension, [Parameter(Mandatory = $true)][scriptblock]$VerifyQueue)
    $fixture = Join-Path $Root ('native-picker-' + [guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $fixture)
    $first = Join-Path $fixture 'first picked file.txt'
    $second = Join-Path $fixture ('second picked file-' + [char]0x03A9 + '.txt')
    $decoy = Join-Path $fixture 'cancelled file must not appear.txt'
    $folder = Join-Path $fixture ('picked folder-' + [char]0x03A9)
    [void](New-Item -ItemType Directory -Path $folder)
    $destination = Join-Path $fixture ('accepted destination-' + [char]0x03A9)
    [void](New-Item -ItemType Directory -Path $destination)
    $cancelledDestination = Join-Path $fixture 'cancelled destination must remain empty'
    [void](New-Item -ItemType Directory -Path $cancelledDestination)
    foreach ($path in @($first, $second, $decoy, (Join-Path $folder 'nested.txt'))) {
        [IO.File]::WriteAllText($path, ('Native picker fixture: ' + [IO.Path]::GetFileName($path)))
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Exe
    $start.UseShellExecute = $false
    foreach ($name in @('FLITZZIP_GUI_SMOKE_FILE_SELECTION', 'FLITZZIP_GUI_SMOKE_FOLDER_SELECTION', 'FLITZZIP_GUI_SMOKE_CLOSE_FILE', 'FLITZZIP_GUI_SMOKE_DESTINATION')) { $start.EnvironmentVariables.Remove($name) }
    $start.EnvironmentVariables['FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS'] = '120000'
    $owned = [Diagnostics.Process]::Start($start)
    $handle = [IntPtr]::Zero
    try {
        $deadline = [datetime]::UtcNow.AddSeconds(15)
        do {
            $owned.Refresh()
            if ($owned.HasExited) { throw 'Native picker GUI exited before showing its window.' }
            if ($owned.MainWindowHandle -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        } while ([datetime]::UtcNow -lt $deadline)
        $handle = $owned.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) { throw 'Native picker GUI did not show its window.' }
        Start-Sleep -Seconds 2
        $dpi = [int][FlitzZipNativeUi]::GetDpiForWindow($handle)
        [void](Invoke-SmokeClientResize -Handle $handle -Dpi $dpi -Width 1200 -Height 760)
        $captures = @()
        $before = $null
        foreach ($scenario in @('Files-Accept', 'Files-Cancel', 'Folder-Accept', 'Folder-Cancel')) {
            Write-Information "Real Windows picker scenario: $scenario" -InformationAction Continue
            $isFolder = $scenario.StartsWith('Folder')
            $cancel = $scenario.EndsWith('Cancel')
            $selection = if ($isFolder) { $folder } elseif ($cancel) { $decoy } else { '"' + $first + '" "' + $second + '"' }
            Invoke-NativePickerSelection -Handle $handle -Dpi $dpi -ProcessId $owned.Id -Selection $selection -Folder:$isFolder -Cancel:$cancel
            $path = "${BasePath}-NativePicker-$scenario$Extension"
            $captures += Save-FlitzZipScreenshot -Handle $handle -Path $path
            if ($cancel) { Assert-PickerQueueUnchanged -Before $before -After $path -Handle $handle -Dpi $dpi }
            $offset = Get-ClientCaptureOffset -Handle $handle
            $lastRow = if ($isFolder) { 2 } else { 1 }
            foreach ($row in 0..$lastRow) {
                Assert-DesignRectHasColor -Path $path -Dpi $dpi -Left 130 -Top (174 + 36 * $row) -Right 146 -Bottom (190 + 36 * $row) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
            }
            $before = $path
        }
        $expected = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
        foreach ($path in @($first, $second)) { $expected[[IO.Path]::GetFileName($path)] = $path }
        $expected[([IO.Path]::GetFileName($folder) + '/nested.txt')] = Join-Path $folder 'nested.txt'
        $captures += Assert-NativePickerDestination -Handle $handle -Dpi $dpi -ProcessId $owned.Id -Destination $destination -Cancelled $cancelledDestination -BasePath $BasePath -Extension $Extension
        $captures += & $VerifyQueue $handle $dpi $destination $expected
        if (@(Get-ChildItem -LiteralPath $cancelledDestination -Force).Count -ne 0) {
            throw 'Cancelling the destination picker redirected archive publication.'
        }
        return $captures
    } finally {
        if (-not $owned.HasExited) {
            if ($handle -ne [IntPtr]::Zero) {
                $popup = [FlitzZipPickerUi]::GetLastActivePopup($handle)
                if ($popup -ne [IntPtr]::Zero -and $popup -ne $handle) {
                    [uint32]$owner = 0
                    [void][FlitzZipPickerUi]::GetWindowThreadProcessId($popup, [ref]$owner)
                    if ($owner -eq $owned.Id) { [void][FlitzZipNativeUi]::PostMessage($popup, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
                }
                [void][FlitzZipNativeUi]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            }
            if (-not $owned.WaitForExit(5000)) { $owned.Kill(); $owned.WaitForExit() }
        }
        $owned.Dispose()
    }
}
Export-ModuleMember -Function Assert-NativePickerQueue
