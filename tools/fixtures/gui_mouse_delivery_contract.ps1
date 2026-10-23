param([Parameter(Mandatory = $true)][string]$SourcePath)

$ErrorActionPreference = 'Stop'
# This transport double tests actual helper dispatch and error cleanup; it does not measure product coverage.
# Real profiled GUI endpoint comparisons separately qualify native Win32 delivery and visible behavior.
Add-Type @'
using System;
using System.Collections.Generic;
public static class FlitzZipNativeUi {
    public static readonly List<string> Calls = new List<string>();
    public static uint RejectMessage;
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
foreach ($name in @('ConvertTo-MouseLParam', 'Send-ClientMouseMessage', 'Invoke-ClientDrag', 'Invoke-ClientClick')) {
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
'GUI mouse delivery contracts passed: exact ordered arguments, bounded acknowledgements, failure propagation and release cleanup.'
