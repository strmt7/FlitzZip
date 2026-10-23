$ErrorActionPreference = 'Stop'

# Purpose: Keep native pointer movement reversible across successful and failing synchronous GUI observations.
# Inputs: A trusted synchronous test action and its explicit arguments; native UI helpers are already loaded.
# Outputs: Forwards action results, restores the original pointer and preserves an original failure on cleanup error.
function Invoke-GuiPointerScope {
    param([scriptblock]$Action, [object[]]$ArgumentList = @())

    [FlitzZipNativeUi+POINT]$previousPointer = New-Object FlitzZipNativeUi+POINT
    if (-not [FlitzZipNativeUi]::GetCursorPos([ref]$previousPointer)) { throw 'Could not preserve the desktop pointer.' }
    $primaryFailure = $null
    try { & $Action @ArgumentList }
    catch { $primaryFailure = $_; throw }
    finally {
        if (-not [FlitzZipNativeUi]::SetCursorPos($previousPointer.X, $previousPointer.Y)) {
            if ($primaryFailure) { Write-Warning 'Could not restore the original desktop pointer; retaining the original test failure.' }
            else { throw 'Could not restore the original desktop pointer.' }
        }
    }
}

# Purpose: Locate an actual painted History resize separator without consulting product state or column arrays.
# Inputs: Validated normal-size capture, monitor DPI/client origin and a narrow design-space search interval.
# Outputs: Measured separator X in design pixels; rejects absent or ambiguous vertical border evidence.
function Get-GuiHistorySeparator {
    param([string]$Path, [int]$Dpi, [int]$OffsetX, [int]$OffsetY, [int]$Left, [int]$Right)

    $bitmap = [Drawing.Bitmap]::FromFile($Path)
    try {
        $scale = $Dpi / 96.0
        $top = $OffsetY + [int][Math]::Round(194 * $scale)
        $bottom = $OffsetY + [int][Math]::Round(214 * $scale)
        $positions = [Collections.Generic.List[int]]::new()
        for ($x = $OffsetX + [int][Math]::Round($Left * $scale); $x -le $OffsetX + [int][Math]::Round($Right * $scale); ++$x) {
            $matching = 0
            for ($y = $top; $y -lt $bottom; ++$y) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -eq 54 -and $color.G -eq 72 -and $color.B -eq 78) { ++$matching }
            }
            if ($matching -ge [int][Math]::Round(16 * $scale)) { $positions.Add($x) }
        }
        if ($positions.Count -eq 0 -or $positions[$positions.Count - 1] - $positions[0] -gt [Math]::Ceiling(2 * $scale)) {
            throw 'A unique painted History separator was not observed.'
        }
        return ($positions[0] - $OffsetX) / $scale
    } finally { $bitmap.Dispose() }
}

# Purpose: Replace a content-only resize check with actual measured column movement and restoration.
# Inputs: Owned normal-size History window, DPI and output prefix with a visible first result row.
# Outputs: Rejects the former non-grip drag as a movement oracle; proves both real drag directions in painted pixels.
function Assert-GuiHistoryColumnResize {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)

    Invoke-GuiPointerScope -ArgumentList @($Handle, $Dpi, $BasePath, $Extension) -Action {
        param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)
        $offset = Get-ClientCaptureOffset -Handle $Handle
        $before = "$BasePath-History-Column-Before$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $before
        $start = Get-GuiHistorySeparator -Path $before -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 320 -Right 370
        Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 430 -DesignY 205
        Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX 430 -StartY 205 -EndX 468 -EndY 205
        $control = "$BasePath-History-Column-NonGrip$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $control
        $unchanged = Get-GuiHistorySeparator -Path $control -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 320 -Right 370
        if ([Math]::Abs($unchanged - $start) -gt 1) { throw 'Non-grip input unexpectedly moved the measured History boundary.' }
        $target = [int][Math]::Round($start) + 40
        Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($start)) -DesignY 205
        Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($start)) -StartY 205 -EndX $target -EndY 205
        $after = "$BasePath-History-AfterColumnResize$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $after
        $moved = Get-GuiHistorySeparator -Path $after -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left ($target - 5) -Right ($target + 5)
        if ([Math]::Abs($moved - $start - 40) -gt 1) { throw 'History column did not move by the independently requested drag distance.' }
        Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($moved)) -DesignY 205
        Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($moved)) -StartY 205 -EndX ([int][Math]::Round($start)) -EndY 205
        $restored = "$BasePath-History-Column-Restored$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $restored
        $end = Get-GuiHistorySeparator -Path $restored -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 320 -Right 370
        if ([Math]::Abs($end - $start) -gt 1) { throw 'History column reverse drag did not restore its measured boundary.' }
    }
}

# Purpose: Exercise a genuinely ellipsized History path through real column resizing, then restore its width.
# Inputs: Owned normal-size History window, DPI and output prefix; the selected result remains unchanged.
# Outputs: Proves narrowed/restored separator positions and actual path tooltip behavior without inventing long state.
function Assert-GuiHistoryPathTooltip {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Prefix, [string]$Extension)

    $offset = Get-ClientCaptureOffset -Handle $Handle
    $before = "$Prefix-Wide$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $before
    $start = Get-GuiHistorySeparator -Path $before -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 620 -Right 670
    $target = [int][Math]::Round($start) + 220
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($start)) -DesignY 205
    Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($start)) -StartY 205 -EndX $target -EndY 205
    $narrow = "$Prefix-Narrow$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $narrow
    $moved = Get-GuiHistorySeparator -Path $narrow -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left ($target - 5) -Right ($target + 5)
    if ([Math]::Abs($moved - $start - 220) -gt 1) { throw 'History path column was not actually narrowed.' }
    Assert-GuiTooltipCycle -Handle $Handle -Dpi $Dpi -X ($target + 40) -Y 238 -Prefix $Prefix -Extension $Extension
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($moved)) -DesignY 205
    Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($moved)) -StartY 205 -EndX ([int][Math]::Round($start)) -EndY 205
    $restored = "$Prefix-Restored$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $restored
    $end = Get-GuiHistorySeparator -Path $restored -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 620 -Right 670
    if ([Math]::Abs($end - $start) -gt 1) { throw 'History path column did not restore its measured boundary.' }
}

# Purpose: Observe the tooltip's unique solid paint without reading product state or changing native timers.
# Inputs: Validated screenshot, monitor DPI and the client origin within that image.
# Outputs: Counts solid tooltip-color samples inside page content; excludes the clock, status strip and title bar.
function Get-GuiTooltipPaintCount {
    param([string]$Path, [int]$Dpi, [int]$OffsetX, [int]$OffsetY)

    $bitmap = [Drawing.Bitmap]::FromFile($Path)
    try {
        $scale = $Dpi / 96.0
        $step = [Math]::Max(1, [int][Math]::Round(3 * $scale))
        $left = $OffsetX + [int][Math]::Round(110 * $scale)
        $top = $OffsetY + [int][Math]::Round(120 * $scale)
        $right = [Math]::Min($bitmap.Width, $OffsetX + [int][Math]::Round(1180 * $scale))
        $bottom = [Math]::Min($bitmap.Height, $OffsetY + [int][Math]::Round(690 * $scale))
        $count = 0
        for ($y = $top; $y -lt $bottom; $y += $step) {
            for ($x = $left; $x -lt $right; $x += $step) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -eq 33 -and $color.G -eq 45 -and $color.B -eq 50) { ++$count }
            }
        }
        return $count
    } finally { $bitmap.Dispose() }
}

# Purpose: Require actual tooltip appearance or disappearance through bounded visual observations.
# Inputs: Owned window/DPI, a same-page tooltip-free baseline, expected visibility and output prefix.
# Outputs: Retains every capture; throws after six observations or eight seconds without the required paint state.
function Save-GuiTooltipExpectation {
    param([IntPtr]$Handle, [int]$Dpi, [int]$Baseline, [bool]$Visible, [string]$Prefix, [string]$Extension)

    $captures = [Collections.Generic.List[object]]::new()
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($observation = 0; $observation -lt 6 -and $watch.Elapsed.TotalSeconds -lt 8; ++$observation) {
        $path = "$Prefix-$observation$Extension"
        $capture = Save-FlitzZipScreenshot -Handle $Handle -Path $path
        $captures.Add($capture)
        $count = Get-GuiTooltipPaintCount -Path $path -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y
        $matched = if ($Visible) { $count -ge $Baseline + 300 } else { $count -le $Baseline + 80 }
        if ($matched) { return $captures.ToArray() }
        Start-Sleep -Milliseconds 120
    }
    throw "Tooltip visibility=$Visible did not match actual paint; baseline=$Baseline observed=$count."
}

# Purpose: Exercise a real delayed tooltip and its cancellation without overriding state or timer results.
# Inputs: Owned window/DPI, eligible design point and a stable same-page empty-space baseline.
# Outputs: Captures actual show/hide and pending-timer cancellation; fails if any tooltip remains or never appears.
function Assert-GuiTooltipCycle {
    param([IntPtr]$Handle, [int]$Dpi, [int]$X, [int]$Y, [string]$Prefix, [string]$Extension)

    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 1100 -DesignY 680
    $idle = Save-FlitzZipScreenshot -Handle $Handle -Path "$Prefix-Idle$Extension"
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $baseline = Get-GuiTooltipPaintCount -Path "$Prefix-Idle$Extension" -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y
    $idle
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX $X -DesignY $Y
    Save-GuiTooltipExpectation -Handle $Handle -Dpi $Dpi -Baseline $baseline -Visible $true -Prefix "$Prefix-Visible" -Extension $Extension
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 1100 -DesignY 680
    Save-GuiTooltipExpectation -Handle $Handle -Dpi $Dpi -Baseline $baseline -Visible $false -Prefix "$Prefix-Hidden" -Extension $Extension
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX $X -DesignY $Y
    Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 1100 -DesignY 680
    Start-Sleep -Milliseconds 650
    Save-GuiTooltipExpectation -Handle $Handle -Dpi $Dpi -Baseline $baseline -Visible $false -Prefix "$Prefix-Cancelled" -Extension $Extension
}

# Purpose: Observe Queue admission before sending a hover that depends on the newly added row.
# Inputs: Owned window/DPI and output prefix after one real file drop.
# Outputs: Retains captures until the first-row name renders; unrelated screenshot or geometry failures propagate.
function Save-GuiHoverQueueReady {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Prefix, [string]$Extension)

    $captures = [Collections.Generic.List[object]]::new()
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($observation = 0; $observation -lt 6 -and $watch.Elapsed.TotalSeconds -lt 8; ++$observation) {
        $path = "$Prefix-$observation$Extension"
        $captures.Add((Save-FlitzZipScreenshot -Handle $Handle -Path $path))
        try {
            Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left 170 -Top 170 -Right 510 -Bottom 190 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
            return $captures.ToArray()
        } catch {
            if ($_.Exception.Message -notlike 'Expected rendered dropdown/detail region*') { throw }
        }
        Start-Sleep -Milliseconds 120
    }
    throw 'Queue hover fixture was not visibly admitted.'
}

# Purpose: Qualify owned hover projections through actual Queue, Extract and selected History rendering.
# Inputs: Real fixture archive/root, normal-size owned window, output prefix and an optional focused Queue/Extract mode.
# Outputs: Visible tooltip/lifetime captures; preserves History selection for subsequent full-smoke consumers.
function Assert-GuiTextTooltip {
    param([IntPtr]$Handle, [int]$Dpi, [string]$FixtureArchive, [string]$FixtureRoot,
        [string]$BasePath, [string]$Extension, [switch]$SkipHistory)

    Invoke-GuiPointerScope -ArgumentList @($Handle, $Dpi, $FixtureArchive, $FixtureRoot, $BasePath, $Extension, [bool]$SkipHistory) -Action {
        param([IntPtr]$Handle, [int]$Dpi, [string]$FixtureArchive, [string]$FixtureRoot,
            [string]$BasePath, [string]$Extension, [bool]$SkipHistory)
        if (-not $SkipHistory) {
            Assert-GuiTooltipCycle -Handle $Handle -Dpi $Dpi -X 450 -Y 238 -Prefix "$BasePath-Hover-History-Name" -Extension $Extension
            Assert-GuiHistoryPathTooltip -Handle $Handle -Dpi $Dpi -Prefix "$BasePath-Hover-History-Path" -Extension $Extension
        }
        $name = 'Hover-' + ([string][char]0x754c * 80) + '.flzip'
        $archive = Join-Path $FixtureRoot $name
        Copy-Item -LiteralPath $FixtureArchive -Destination $archive -Force
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($archive)
        Save-GuiHoverQueueReady -Handle $Handle -Dpi $Dpi -Prefix "$BasePath-Hover-Queue-Ready" -Extension $Extension
        Assert-GuiTooltipCycle -Handle $Handle -Dpi $Dpi -X 250 -Y 180 -Prefix "$BasePath-Hover-Queue-Name" -Extension $Extension
        Assert-GuiTooltipCycle -Handle $Handle -Dpi $Dpi -X 850 -Y 180 -Prefix "$BasePath-Hover-Queue-Path" -Extension $Extension
        # Keyboard activation removes the row while the physical pointer remains over its former text cell.
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($tab = 0; $tab -lt 11; ++$tab) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x09 }
        Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 250 -DesignY 180
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
        Start-Sleep -Milliseconds 650
        Save-GuiTooltipExpectation -Handle $Handle -Dpi $Dpi -Baseline 0 -Visible $false -Prefix "$BasePath-Hover-Removed" -Extension $Extension
        $empty = "$BasePath-Hover-Removed-Empty$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $empty
        $offset = Get-ClientCaptureOffset -Handle $Handle
        Assert-QueueEmptyMessageCentered -Path $empty -Dpi $Dpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($archive)
        Save-GuiHoverQueueReady -Handle $Handle -Dpi $Dpi -Prefix "$BasePath-Hover-Queue-Restored" -Extension $Extension
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
        Assert-GuiTooltipCycle -Handle $Handle -Dpi $Dpi -X 250 -Y 160 -Prefix "$BasePath-Hover-Extract-Archive" -Extension $Extension
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX 250 -DesignY 160
        Start-Sleep -Milliseconds 650
        Save-GuiTooltipExpectation -Handle $Handle -Dpi $Dpi -Baseline 0 -Visible $false -Prefix "$BasePath-Hover-Untruncated" -Extension $Extension
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4 -Synchronous
    }
}

Export-ModuleMember -Function Assert-GuiTextTooltip, Assert-GuiHistoryColumnResize
