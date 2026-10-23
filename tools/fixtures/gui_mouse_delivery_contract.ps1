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
    public static bool RejectMapping;
    public static bool RejectPointer;
    public static bool RejectClient;
    public static bool RejectPointerRead;
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
        return message == RejectMessage ? IntPtr.Zero : new IntPtr(1);
    }
    // Purpose: Reject unacknowledged delivery. Inputs: Unused scalar message arguments.
    // Outputs: Throws when an actual helper uses this unsupported test transport.
    public static bool PostMessage(IntPtr handle, uint message, IntPtr keys, IntPtr position) {
        throw new InvalidOperationException("Unacknowledged input cannot establish completed drag delivery.");
    }
}
'@
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($SourcePath, [ref]$null, [ref]$errors)
if ($errors.Count) { throw 'GUI helper source failed parsing.' }
foreach ($name in @('ConvertTo-MouseLParam', 'Send-ClientMouseMessage', 'Invoke-ClientDrag', 'Invoke-ClientClick', 'Invoke-ClientMove')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $definition -and $name -eq 'Send-ClientMouseMessage') { continue }
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
        if ($_.Exception.Message -notlike 'GUI mouse input failed or timed out;*') { throw }
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
    if ($_.Exception.Message -notlike 'GUI mouse input failed or timed out;*') { throw }
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
