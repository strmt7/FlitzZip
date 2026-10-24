param([Parameter(Mandatory = $true)][string]$SourcePath,
    [string]$HoverSourcePath = (Join-Path (Split-Path -Parent $SourcePath) 'FlitzZip.GuiSmoke.Hover.psm1'))

$ErrorActionPreference = 'Stop'
# This transport double tests actual helper dispatch and error cleanup; it does not measure product coverage.
# Real profiled GUI endpoint comparisons separately qualify native Win32 delivery and visible behavior.
Add-Type @'
using System;
using System.Collections.Generic;
public static class FlitzZipNativeUi {
    public static readonly List<string> Calls = new List<string>();
    public static uint RejectMessage;
    public static uint RejectOtherMessage;
    public static bool RejectMapping;
    public static bool RejectPointer;
    public static bool RejectClient;
    public static bool RejectPointerRead;
    public static bool RejectDropAllocation;
    public static bool RejectDropFree;
    // Purpose: Model an independent owned payload. Inputs: Paths and scaled point. Outputs: Handle or allocation failure.
    public static IntPtr CreateDropHandle(string[] paths, int x, int y) {
        Calls.Add(String.Format("drop:{0}:{1}:{2}", paths.Length, x, y));
        if (RejectDropAllocation) throw new InvalidOperationException("drop-allocation-failure");
        return new IntPtr(900);
    }
    // Purpose: Verify rejected payload ownership. Inputs: Original handle. Outputs: Acknowledged free or retained failure.
    public static IntPtr GlobalFree(IntPtr handle) {
        Calls.Add("free:" + handle.ToInt64());
        return RejectDropFree ? handle : IntPtr.Zero;
    }
    public struct POINT { public int X; public int Y; }
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    // Purpose: Supply independent client bounds. Inputs: Owned test HWND. Outputs: A fixed bounded rectangle.
    public static bool GetClientRect(IntPtr handle, out RECT rect) {
        Calls.Add("client:" + handle.ToInt64());
        rect = new RECT { Right = 1200, Bottom = 760 };
        return !RejectClient;
    }
    // Purpose: Record DPI-mapped client input. Inputs: HWND and point. Outputs: Independent screen translation.
    public static bool ClientToScreen(IntPtr handle, ref POINT point) {
        Calls.Add(String.Format("map:{0}:{1}:{2}", handle.ToInt64(), point.X, point.Y));
        point.X += 100; point.Y += 200;
        return !RejectMapping;
    }
    // Purpose: Require real pointer positioning before acknowledgement. Inputs: Screen point.
    // Outputs: Records the position and optionally rejects native movement.
    public static bool SetCursorPos(int x, int y) {
        Calls.Add(String.Format("pointer:{0}:{1}", x, y));
        return !RejectPointer;
    }
    // Purpose: Preserve an independently chosen original pointer. Inputs: Writable point.
    // Outputs: Records access and returns fixed coordinates, or rejects unavailable native input.
    public static bool GetCursorPos(out POINT point) {
        Calls.Add("read-pointer"); point = new POINT { X = 17, Y = 29 };
        return !RejectPointerRead;
    }
    // Purpose: Record transport arguments and inject acknowledgement failure. Inputs: Scalar Win32 arguments.
    // Outputs: Stores one call and returns the configured acknowledgement; no native window is accessed.
    public static IntPtr SendMessageTimeout(IntPtr handle, uint message, IntPtr keys, IntPtr position,
                                            uint flags, uint timeout, out UIntPtr result) {
        Calls.Add(String.Format("{0}:{1}:{2}:{3}:{4}:{5}", handle.ToInt64(), message, keys.ToInt64(),
                                position.ToInt64(), flags, timeout));
        result = UIntPtr.Zero;
        return message == RejectMessage || message == RejectOtherMessage ? IntPtr.Zero : new IntPtr(1);
    }
    // Purpose: Detect an unbounded keyboard transport without blocking the test process.
    // Inputs: The actual helper's Win32 message arguments. Outputs: Rejects a call with no acknowledgement deadline.
    public static IntPtr SendMessage(IntPtr handle, uint message, IntPtr keys, IntPtr position) {
        throw new InvalidOperationException("Unbounded SendMessage cannot qualify keyboard delivery.");
    }
    // Purpose: Record modal-launch queue admission. Inputs: Scalar native input and configured rejection.
    // Outputs: Records queue-only delivery, independently distinct from completion acknowledgement.
    public static bool PostMessage(IntPtr handle, uint message, IntPtr keys, IntPtr position) {
        Calls.Add(String.Format("queued:{0}:{1}:{2}:{3}", handle.ToInt64(), message, keys.ToInt64(), position.ToInt64()));
        return message != RejectMessage && message != RejectOtherMessage;
    }
}
'@
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($SourcePath, [ref]$null, [ref]$errors)
if ($errors.Count) { throw 'GUI helper source failed parsing.' }
foreach ($name in @('Get-GuiSmokeWatchdogInterval', 'ConvertTo-MouseLParam', 'Send-ClientMouseMessage', 'Send-ClientInputMessage', 'Complete-ClientInputRelease', 'Invoke-ClientDrag', 'Invoke-ClientClick', 'Invoke-ClientMove', 'Invoke-ClientKey', 'Invoke-ClientWheel', 'Invoke-FileDrop')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $definition -and $name -in @('Send-ClientMouseMessage', 'Send-ClientInputMessage', 'Complete-ClientInputRelease')) { continue }
    if (-not $definition) { throw "GUI delivery helper is missing: $name" }
    . ([ScriptBlock]::Create($definition.Extent.Text))
}

# Purpose: Require exact ordered transport arguments without replacing any production helper body.
# Inputs: Expected contains independently calculated HWND/message/key/DPI-position/bound tuples.
# Outputs: Throws if an input is omitted, reordered, unbounded or sent through the unacknowledged transport.
function Assert-DeliverySequence {
    param([string[]]$Expected)
    if (([FlitzZipNativeUi]::Calls -join '|') -cne ($Expected -join '|')) {
        throw 'GUI mouse delivery did not preserve the expected bounded input sequence.'
    }
}

$down = '42:513:1:1966095:2:5000'
$move = '42:512:1:3932205:2:5000'
$up = '42:514:0:3932205:2:5000'
$drag = @{ Handle = [IntPtr]42; Dpi = 144; StartX = 10; StartY = 20; EndX = 30; EndY = 40 }
Invoke-ClientDrag @drag
Assert-DeliverySequence -Expected @($down, $move, $up)
foreach ($rejectedMessage in @(513, 512, 514)) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMessage = $rejectedMessage
    $rejected = $false
    try { Invoke-ClientDrag @drag }
    catch {
        if ($_.Exception.Message -notlike 'GUI *input failed or timed out;*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'A failed native acknowledgement was accepted as completed drag input.' }
    $expected = if ($rejectedMessage -eq 513) { @($down, $up) } else { @($down, $move, $up) }
    Assert-DeliverySequence -Expected $expected
}
[FlitzZipNativeUi]::Calls.Clear()
[FlitzZipNativeUi]::RejectMessage = 0
Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 -Synchronous
Assert-DeliverySequence -Expected @($down, '42:514:0:1966095:2:5000')
[FlitzZipNativeUi]::Calls.Clear()
Invoke-ClientMove -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20
Assert-DeliverySequence -Expected @('client:42', 'map:42:15:30', 'pointer:115:230', '42:512:0:1966095:2:5000')
[FlitzZipNativeUi]::Calls.Clear()
[FlitzZipNativeUi]::RejectMessage = 512
$rejected = $false
try { Invoke-ClientMove -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 }
catch {
    if ($_.Exception.Message -notlike 'GUI *input failed or timed out;*') { throw }
    $rejected = $true
}
if (-not $rejected) { throw 'Unacknowledged pointer movement was admitted.' }
Assert-DeliverySequence -Expected @('client:42', 'map:42:15:30', 'pointer:115:230', '42:512:0:1966095:2:5000')
[FlitzZipNativeUi]::RejectMessage = 0
foreach ($failure in @('Client', 'Mapping', 'Pointer', 'Bounds', 'Dpi')) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMapping = $failure -eq 'Mapping'
    [FlitzZipNativeUi]::RejectPointer = $failure -eq 'Pointer'
    [FlitzZipNativeUi]::RejectClient = $failure -eq 'Client'
    $rejected = $false
    try { Invoke-ClientMove -Handle ([IntPtr]42) -Dpi $(if ($failure -eq 'Dpi') { 0 } else { 144 }) -DesignX $(if ($failure -eq 'Bounds') { 1200 } else { 10 }) -DesignY 20 }
    catch {
        if ($_.Exception.Message -notin @('Could not map the owned hover point.', 'Could not move the native hover pointer.', 'Pointer movement must stay inside the owned client rectangle.', 'Pointer movement requires a positive window DPI.')) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "Invalid native pointer movement was admitted: $failure" }
    $expected = switch ($failure) {
        'Dpi' { @() }
        'Client' { @('client:42') }
        'Bounds' { @('client:42') }
        'Mapping' { @('client:42', 'map:42:15:30') }
        'Pointer' { @('client:42', 'map:42:15:30', 'pointer:115:230') }
    }
    Assert-DeliverySequence -Expected $expected
}

$hoverErrors = $null
$hoverAst = [Management.Automation.Language.Parser]::ParseFile($HoverSourcePath, [ref]$null, [ref]$hoverErrors)
if ($hoverErrors.Count) { throw 'GUI hover source failed parsing.' }
$pointerScope = $hoverAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-GuiPointerScope'
}, $true)
if (-not $pointerScope) { throw 'Actual pointer ownership scope is missing.' }
. ([ScriptBlock]::Create($pointerScope.Extent.Text))
foreach ($failure in @('None', 'Read', 'Action', 'Restore', 'ActionAndRestore')) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectPointerRead = $failure -eq 'Read'
    [FlitzZipNativeUi]::RejectPointer = $failure -in @('Restore', 'ActionAndRestore')
    $observed = $null
    try {
        $value = Invoke-GuiPointerScope -ArgumentList @('owned-result', $failure) -Action {
            param([string]$result, [string]$failureKind)
            [FlitzZipNativeUi]::Calls.Add('action')
            if ($failureKind -in @('Action', 'ActionAndRestore')) { throw 'controlled original action failure' }
            $result
        }
    } catch { $observed = $_.Exception.Message }
    $expectedError = switch ($failure) {
        'None' { $null }
        'Read' { 'Could not preserve the desktop pointer.' }
        'Restore' { 'Could not restore the original desktop pointer.' }
        default { 'controlled original action failure' }
    }
    if ($observed -cne $expectedError) { throw "Pointer ownership lost the original result or failure: $failure" }
    if ($failure -eq 'None' -and $value -cne 'owned-result') { throw 'Pointer ownership changed the action output.' }
    $expectedCalls = if ($failure -eq 'Read') { @('read-pointer') } else { @('read-pointer', 'action', 'pointer:17:29') }
    Assert-DeliverySequence -Expected $expectedCalls
}
'GUI mouse delivery contracts passed: exact ordered arguments, bounded acknowledgements, failure propagation and release cleanup.'

# Keyboard acknowledgements must retain the exact HWND, VK and deadline and release even after a rejected press.
[FlitzZipNativeUi]::RejectMessage = 0
[FlitzZipNativeUi]::Calls.Clear()
Invoke-ClientKey -Handle ([IntPtr]42) -VirtualKey 0x20
$keyDown = '42:256:32:0:2:5000'
$keyUp = '42:257:32:0:2:5000'
Assert-DeliverySequence -Expected @($keyDown, $keyUp)
foreach ($rejectedMessage in @(256, 257)) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMessage = $rejectedMessage
    $rejected = $false
    try { Invoke-ClientKey -Handle ([IntPtr]42) -VirtualKey 0x20 }
    catch {
        if ($_.Exception.Message -notlike 'GUI *input failed or timed out;*') { throw }
        if (-not $_.Exception.Message.Contains(('message=0x{0:X4}' -f $rejectedMessage))) {
            throw 'Keyboard delivery reported a different message failure.'
        }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Unacknowledged keyboard input was accepted.' }
    Assert-DeliverySequence -Expected @($keyDown, $keyUp)
}
[FlitzZipNativeUi]::RejectMessage = 256
[FlitzZipNativeUi]::RejectOtherMessage = 257
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
$cleanupWarnings = @()
try { Invoke-ClientKey -Handle ([IntPtr]42) -VirtualKey 0x20 -WarningVariable cleanupWarnings -WarningAction SilentlyContinue }
catch {
    if ($_.Exception.Message -notlike 'GUI *input failed or timed out;*') { throw }
    $rejected = $true
}
if (-not $rejected -or $cleanupWarnings.Count -ne 1) { throw 'Keyboard cleanup lost the original failure or its release warning.' }
Assert-DeliverySequence -Expected @($keyDown, $keyUp)
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
try { Invoke-ClientKey -Handle ([IntPtr]42) -VirtualKey 0x20 -WarningAction Stop }
catch {
    if (-not $_.Exception.Message.Contains('message=0x0100') -or
        -not $_.Exception.Data['InputReleaseFailure'].Contains('message=0x0101')) {
        throw 'A warning preference replaced the primary keyboard failure or lost the secondary failure.'
    }
    $rejected = $true
}
if (-not $rejected) { throw 'Combined keyboard failure was lost with stopping warnings.' }
Assert-DeliverySequence -Expected @($keyDown, $keyUp)
[FlitzZipNativeUi]::RejectMessage = 0
[FlitzZipNativeUi]::RejectOtherMessage = 0
'Keyboard delivery uses bounded acknowledgements and preserves press/release failures.'

# The same ownership boundary governs default clicks and callbacks during captured drags.
[FlitzZipNativeUi]::Calls.Clear()
Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20
Assert-DeliverySequence -Expected @($down, '42:514:0:1966095:2:5000')
foreach ($rejectedMessage in @(513, 514)) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMessage = $rejectedMessage
    $rejected = $false
    try { Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 }
    catch {
        if (-not $_.Exception.Message.Contains(('message=0x{0:X4}' -f $rejectedMessage))) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Default click lost a native acknowledgement failure.' }
    Assert-DeliverySequence -Expected @($down, '42:514:0:1966095:2:5000')
}
[FlitzZipNativeUi]::RejectMessage = 0
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
try { Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 -Synchronous:$false }
catch { if ($_.Exception.Message -ne 'GUI click acknowledgement cannot be disabled.') { throw }; $rejected = $true }
if (-not $rejected) { throw 'A caller disabled click completion.' }
Assert-DeliverySequence -Expected @()
[FlitzZipNativeUi]::Calls.Clear()
Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 -ModalLaunch
Assert-DeliverySequence -Expected @('queued:42:513:1:1966095', 'queued:42:514:0:1966095')
foreach ($rejectedMessage in @(513, 514)) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMessage = $rejectedMessage
    $rejected = $false
    try { Invoke-ClientClick -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 -ModalLaunch }
    catch {
        if (-not $_.Exception.Message.Contains(('GUI input queue admission failed; message=0x{0:X4}' -f $rejectedMessage))) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Modal launch lost a queue admission failure.' }
    Assert-DeliverySequence -Expected @('queued:42:513:1:1966095', 'queued:42:514:0:1966095')
}
[FlitzZipNativeUi]::RejectMessage = 0
[FlitzZipNativeUi]::Calls.Clear()
$value = Invoke-ClientDrag @drag -DuringDrag {
    param($marker)
    [FlitzZipNativeUi]::Calls.Add($marker)
    'callback-result'
} -DuringDragArgumentList @('action')
if ($value -cne 'callback-result') { throw 'A captured callback lost its output or explicit arguments.' }
Assert-DeliverySequence -Expected @($down, 'action', $move, $up)
foreach ($releaseRejected in @($false, $true)) {
    [FlitzZipNativeUi]::Calls.Clear()
    [FlitzZipNativeUi]::RejectMessage = if ($releaseRejected) { 514 } else { 0 }
    $rejected = $false
    try {
        Invoke-ClientDrag @drag -DuringDrag {
            [FlitzZipNativeUi]::Calls.Add('action')
            throw 'captured-action-failure'
        } -WarningAction Stop
    } catch {
        if ($_.Exception.Message -cne 'captured-action-failure') { throw }
        if ($releaseRejected -and -not $_.Exception.Data['InputReleaseFailure'].Contains('message=0x0202')) {
            throw 'The secondary mouse release diagnostic was lost.'
        }
        $rejected = $true
    }
    if (-not $rejected) { throw 'A captured callback failure was accepted.' }
    Assert-DeliverySequence -Expected @($down, 'action', $up)
}
[FlitzZipNativeUi]::RejectMessage = 0
foreach ($delta in @(-120, 120)) {
    [FlitzZipNativeUi]::Calls.Clear()
    Invoke-ClientWheel -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 -Delta $delta
    $expectedWheel = if ($delta -lt 0) { '42:522:-7864320:15073395:2:5000' } else { '42:522:7864320:15073395:2:5000' }
    Assert-DeliverySequence -Expected @('map:42:15:30', $expectedWheel)
}
[FlitzZipNativeUi]::RejectMessage = 522
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
try { Invoke-ClientWheel -Handle ([IntPtr]42) -Dpi 144 -DesignX 10 -DesignY 20 }
catch { if (-not $_.Exception.Message.Contains('message=0x020A')) { throw }; $rejected = $true }
if (-not $rejected) { throw 'A rejected wheel acknowledgement was accepted.' }
Assert-DeliverySequence -Expected @('map:42:15:30', '42:522:-7864320:15073395:2:5000')
[FlitzZipNativeUi]::RejectMessage = 0
'All scalar input, captured callbacks, release failures and wheel coordinates passed.'

[FlitzZipNativeUi]::RejectPointer = $false
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
try {
    Invoke-GuiPointerScope -Action {
        [FlitzZipNativeUi]::Calls.Add('action')
        [FlitzZipNativeUi]::RejectPointer = $true
        throw 'pointer-action-primary-failure'
    } -WarningAction Stop
} catch {
    if ($_.Exception.Message -cne 'pointer-action-primary-failure' -or
        $_.Exception.Data['PointerRestoreFailure'] -cne 'Could not restore the original desktop pointer.') { throw }
    $rejected = $true
}
if (-not $rejected) { throw 'Pointer cleanup hid the primary failure under stopping warnings.' }
Assert-DeliverySequence -Expected @('read-pointer', 'action', 'pointer:17:29')
[FlitzZipNativeUi]::RejectPointer = $false
'Pointer restoration preserves both errors under stopping warnings.'

[FlitzZipNativeUi]::Calls.Clear()
Invoke-FileDrop -Handle ([IntPtr]42) -Dpi 144 -Paths @('C:\first.txt', 'C:\second.txt')
Assert-DeliverySequence -Expected @('drop:2:450:480', 'queued:42:563:900:0')
foreach ($cleanupFails in @($false, $true)) {
    [FlitzZipNativeUi]::RejectMessage = 563
    [FlitzZipNativeUi]::RejectDropFree = $cleanupFails
    [FlitzZipNativeUi]::Calls.Clear()
    $rejected = $false
    try { Invoke-FileDrop -Handle ([IntPtr]42) -Dpi 144 -Paths @() -WarningAction Stop }
    catch {
        if ($_.Exception.Message -notlike 'GUI input queue admission failed; message=0x0233;*') { throw }
        if ($cleanupFails -ne $_.Exception.Data.Contains('DropPayloadCleanupFailure')) {
            throw 'HDROP cleanup lost or invented a secondary failure.'
        }
        $rejected = $true
    }
    if (-not $rejected) { throw 'An undelivered HDROP payload was reported as accepted.' }
    Assert-DeliverySequence -Expected @('drop:0:450:480', 'queued:42:563:900:0', 'free:900')
}
[FlitzZipNativeUi]::RejectMessage = 0
[FlitzZipNativeUi]::RejectDropFree = $false
[FlitzZipNativeUi]::RejectDropAllocation = $true
[FlitzZipNativeUi]::Calls.Clear()
$rejected = $false
try { Invoke-FileDrop -Handle ([IntPtr]42) -Paths @('C:\input.txt') }
catch { if (-not $_.Exception.Message.Contains('drop-allocation-failure')) { throw }; $rejected = $true }
if (-not $rejected) { throw 'HDROP allocation failure was accepted.' }
Assert-DeliverySequence -Expected @('drop:1:300:320')
'HDROP delivery transfers or releases ownership exactly once and preserves both failures.'

# Exercise actual picker readiness against delayed initialization without opening a window.
Add-Type @'
using System;
using System.Text;
public static class FlitzZipPickerUi {
    public static int Attempts;
    public static int VisibleAfter = 3, EnabledAfter = 4;
    public static bool DelayInner, ForeignEdit, Closed;
    // Purpose: Model child creation before readiness. Inputs: Dialog and ID. Outputs: Stable test HWND.
    public static IntPtr GetDlgItem(IntPtr dialog, int id) { ++Attempts; return new IntPtr(id == 1152 ? 52 : 48); }
    // Purpose: Model delayed edit creation. Inputs: Parent/class. Outputs: Child or no HWND yet.
    public static IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string name) {
        if (DelayInner && Attempts < 5) return IntPtr.Zero;
        return new IntPtr(cls == "ComboBox" ? 49 : 50);
    }
    // Purpose: Model hidden initialization. Inputs: HWND. Outputs: Visibility at its independent threshold.
    public static bool IsWindowVisible(IntPtr hwnd) { return Attempts >= VisibleAfter; }
    // Purpose: Model an unready field. Inputs: HWND. Outputs: Enablement at its independent threshold.
    public static bool IsWindowEnabled(IntPtr hwnd) { return Attempts >= EnabledAfter; }
    // Purpose: Model modal destruction. Inputs: HWND. Outputs: Whether initialization can continue.
    public static bool IsWindow(IntPtr hwnd) { return !Closed; }
    // Purpose: Supply ownership evidence. Inputs: HWND. Outputs: Owner or foreign edit process.
    public static uint GetWindowThreadProcessId(IntPtr hwnd, out uint owner) {
        owner = ForeignEdit && (hwnd.ToInt32() == 50 || hwnd.ToInt32() == 52) ? 99u : 42u;
        return 1;
    }
    // Purpose: Supply class identity. Inputs: HWND. Outputs: Dialog/container/edit class.
    public static int GetClassName(IntPtr hwnd, StringBuilder name, int capacity) {
        int id = hwnd.ToInt32();
        string value = id == 10 ? "#32770" : id == 48 ? "ComboBoxEx32" : id == 49 ? "ComboBox" : "Edit";
        name.Append(value); return value.Length;
    }
}
'@
$pickerPath = Join-Path (Split-Path -Parent $SourcePath) 'FlitzZip.GuiSmoke.Pickers.psm1'
$pickerAst = [Management.Automation.Language.Parser]::ParseFile($pickerPath, [ref]$null, [ref]$errors)
if ($errors.Count) { throw 'Native picker helper source failed parsing.' }
foreach ($name in @('Assert-PickerWindowOwner', 'Wait-PickerInputControl')) {
    $definition = $pickerAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $definition) { throw "Native picker readiness helper missing: $name" }
    . ([ScriptBlock]::Create($definition.Extent.Text))
}
foreach ($folder in @($false, $true)) {
    foreach ($delayedChild in @($false, $true)) {
        foreach ($thresholds in @(@(3, 4), @(5, 1), @(1, 4))) {
            [FlitzZipPickerUi]::Attempts = 0
            [FlitzZipPickerUi]::DelayInner = $delayedChild
            [FlitzZipPickerUi]::VisibleAfter = $thresholds[0]
            [FlitzZipPickerUi]::EnabledAfter = $thresholds[1]
            $actual = Wait-PickerInputControl -Dialog ([IntPtr]10) -ProcessId 42 -Folder $folder
            $expectedHandle = if ($folder) { 52 } else { 50 }
            $expectedPolls = [Math]::Max($thresholds[0], $thresholds[1])
            if ($delayedChild -and -not $folder) { $expectedPolls = [Math]::Max($expectedPolls, 5) }
            if ($actual.ToInt32() -ne $expectedHandle -or [FlitzZipPickerUi]::Attempts -ne $expectedPolls) {
                throw 'Native picker admitted a hidden, disabled or uninitialized child control.'
            }
        }
    }
    [FlitzZipPickerUi]::Attempts = 0
    [FlitzZipPickerUi]::ForeignEdit = $true
    $rejected = $false
    try { [void](Wait-PickerInputControl -Dialog ([IntPtr]10) -ProcessId 42 -Folder $folder) }
    catch { if (-not $_.Exception.Message.Contains('ownership/class validation failed')) { throw }; $rejected = $true }
    finally { [FlitzZipPickerUi]::ForeignEdit = $false }
    if (-not $rejected) { throw 'Native picker admitted an edit from another process.' }
}
[FlitzZipPickerUi]::Attempts = 0
[FlitzZipPickerUi]::Closed = $true
$rejected = $false
try { [void](Wait-PickerInputControl -Dialog ([IntPtr]10) -ProcessId 42 -Folder $true) }
catch { if (-not $_.Exception.Message.Contains('closed before its input became interactive')) { throw }; $rejected = $true }
if (-not $rejected) { throw 'Native picker waited on a destroyed dialog.' }
'Native picker readiness rejects early, foreign and destroyed controls.'

# The child fuse must not close an active scenario before the process owner's deadline.
foreach ($budgetCase in @(@(900, 10000, 910000), @(17, 2000, 19000), @(3600, 60000, 3660000))) {
    $actualBudget = Get-GuiSmokeWatchdogInterval -ScenarioTimeoutSeconds $budgetCase[0] -CleanupTimeoutMilliseconds $budgetCase[1]
    if ($actualBudget -ne $budgetCase[2] -or $actualBudget -le ($budgetCase[0] * 1000)) {
        throw 'GUI watchdog can terminate an active scenario before its owning deadline and cleanup grace.'
    }
}
foreach ($invalidBudget in @(@(0, 10000), @(3601, 10000), @(900, 0), @(900, 60001))) {
    $rejectedBudget = $false
    try { [void](Get-GuiSmokeWatchdogInterval -ScenarioTimeoutSeconds $invalidBudget[0] -CleanupTimeoutMilliseconds $invalidBudget[1]) }
    catch [Management.Automation.ParameterBindingException] { $rejectedBudget = $true }
    if (-not $rejectedBudget) { throw 'GUI watchdog accepted an unbounded or nonpositive scenario/cleanup budget.' }
}
'GUI watchdog follows its owning scenario budget and retains bounded cleanup grace.'
