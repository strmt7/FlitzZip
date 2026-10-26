param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$ScreenshotPath = "",
    [switch]$CompactOnly,
    [switch]$OperationSummaryOnly,
    [switch]$HoverOnly,
    [switch]$StatusFeedbackOnly,
    [ValidateRange(1, 3600)][int]$ScenarioTimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'process_environment.ps1')
. (Join-Path $PSScriptRoot 'gui_settings_choices.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Configuration\FlitzZip.exe"
if (-not (Test-Path $exe)) {
    throw "FlitzZip.exe not found. Run tools/build.ps1 first."
}
if ([string]::IsNullOrWhiteSpace($ScreenshotPath)) {
    $ScreenshotPath = Join-Path $repo "out\gui-smoke-$Configuration.png"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ScreenshotPath) | Out-Null

# Purpose: Load GUI source text for source-contract assertions.
# Inputs: None; reads the repository app source tree.
# Outputs: Returns a single joined text buffer for policy checks.
function Get-GuiSourceText {
    $appSources = Get-ChildItem -LiteralPath (Join-Path $repo "src\app") -File -Recurse -Include *.cpp, *.hpp, *.h, *.rc
    return ($appSources | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
}

# Purpose: Inspect C++/resource string tokens without confusing comments, character literals or identifiers with UI text.
# Inputs: Joined, compilable GUI source; this lexical check supplements actual native GUI assertions.
# Outputs: String-token spellings only, retaining prefixes and escapes for the existing exact-label contracts.
function Get-GuiLiteralText {
    param([Parameter(Mandatory = $true)][string]$SourceText)

    $tokenPattern = @'
//(?:\\\r?\n|[^\r\n])*|/\*.*?(?:\*/|\z)|(?<literal>(?:u8|u|U|L)?R"(?<delimiter>[^\s()\\]{0,16})\(.*?\)\k<delimiter>"|(?:u8|u|U|L)?"(?:\\.|[^"\\])*")|(?<![\p{L}\p{N}_])(?:u8|u|U|L)?'(?:\\.|[^'\\\r\n])*'
'@
    $tokens = [regex]::Matches($SourceText, $tokenPattern, [Text.RegularExpressions.RegexOptions]::Singleline, [TimeSpan]::FromSeconds(5))
    return (($tokens | Where-Object { $_.Groups['literal'].Success } | ForEach-Object { $_.Value }) -join "`n")
}

# Purpose: Fail fast when GUI literals reintroduce previously rejected user-facing labels.
# Inputs: `SourceText` is the joined GUI source text; comments and non-string tokens are not UI labels.
# Outputs: Throws with a precise remediation message when a banned literal appears.
function Assert-GuiBlockedText {
    param([Parameter(Mandatory = $true)][string]$SourceText)

    $literalText = Get-GuiLiteralText -SourceText $SourceText
    $blockedPatterns = @(
        @{ Pattern = ('\bleve' + 'ls\b'); Message = "GUI source must not use plural compression-setting wording; compression labels are named options." },
        @{ Pattern = ('Verbose ' + 'diagnostics'); Message = "GUI log level label must be 'Debug'." },
        @{ Pattern = ('\bWarn' + 'ings\b'); Message = "GUI log level label must be singular 'Warning'." },
        @{ Pattern = ('Session ' + 'only'); Message = "GUI log retention options must be exactly '1 week', '2 weeks', and '1 month'." },
        @{ Pattern = ('Current ' + 'session'); Message = "GUI log retention options must not reintroduce the retired current-session option." },
        @{ Pattern = ('\b7 ' + 'days\b'); Message = "GUI log retention options must use '1 week', not '7 days'." },
        @{ Pattern = ('\b30 ' + 'days\b'); Message = "GUI log retention options must use '1 month', not '30 days'." },
        @{ Pattern = ('AMD GPU ' + 'Diagnostics'); Message = "The former GPU page title must remain 'System'." },
        @{ Pattern = ('L"Archive ' + 'format"'); Message = "Compress and Extract pages must label archive selectors as 'Format'." },
        @{ Pattern = ('Format-' + 'managed'); Message = "Unsupported compression options must render as disabled '-' fields." },
        @{ Pattern = ('Native Windows AMD HIP ' + 'archive utility'); Message = "The About page must use the canonical product tagline." },
        @{ Pattern = ('L"Det' + 'ails"'); Message = "The status bar must show the clock instead of the retired Details label." }
    )
    foreach ($rule in $blockedPatterns) {
        if ($literalText -cmatch $rule.Pattern) {
            throw $rule.Message
        }
    }
    foreach ($requiredLabel in @('L"1 week"', 'L"2 weeks"', 'L"1 month"')) {
        if (-not $literalText.Contains($requiredLabel)) {
            throw "GUI log retention option missing required label $requiredLabel."
        }
    }
    if ($literalText -cmatch 'L"Session"') {
        throw "History rows must use the real completion time, not the literal Session label."
    }
}

# Purpose: Exercise row and header selection without allowing dynamic buttons to steal keyboard focus.
# Inputs: `Handle`/`Dpi` identify the owned window, `InputPath` is a fixture, and `BasePath`/`Extension` locate captures.
# Outputs: Returns selection and navigation captures; fails if keyboard input loses or activates the wrong control.
function Assert-KeyboardQueueSelection {
    param([IntPtr]$Handle, [int]$Dpi, [string]$InputPath, [string]$BasePath, [string]$Extension)

    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
    Invoke-FileDrop -Handle $Handle -Paths @($InputPath)
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $initial = "${BasePath}-Keyboard-Initial$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $initial
    Assert-DesignRectHasColor -Path $initial -Dpi $Dpi -Left 130 -Top 174 -Right 146 -Bottom 190 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
    foreach ($target in @('Row', 'Header')) {
        if ($target -eq 'Row') {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
        } else {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
            # Eight navigation items, then Remove/Add files/Add folder/Clear, then select-all.
            for ($tab = 0; $tab -lt 12; ++$tab) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x09 }
        }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
        $unchecked = "${BasePath}-Keyboard-$target-Unchecked$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $unchecked
        $focusRect = if ($target -eq 'Row') {
            @{ Left = 130; Top = 165; Right = 300; Bottom = 169 }
        } else {
            @{ Left = 125; Top = 133; Right = 150; Bottom = 158 }
        }
        Assert-DesignRectHasColor -Path $unchecked -Dpi $Dpi @focusRect -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
        $checked = "${BasePath}-Keyboard-$target-Rechecked$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $checked
        Assert-DesignRectHasColor -Path $checked -Dpi $Dpi -Left 130 -Top 174 -Right 146 -Bottom 190 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
    }
    foreach ($page in @(1, 4, 5)) {
        # Nonempty Queue and History lists must not swallow arrows while a navigation item has focus.
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($step = 0; $step -lt $page; ++$step) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        $navigation = "${BasePath}-Keyboard-Page-$page$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $navigation
        Assert-DesignRectHasColor -Path $navigation -Dpi $Dpi -Left 0 -Top (70 + 63 * $page) -Right 4 -Bottom (110 + 63 * $page) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
}

# Purpose: Apply a smoke scenario's required diagnostic level through the actual Settings controls.
# Inputs: Owned normal-size GUI, isolated settings file and validated log-level index.
# Outputs: Persists and independently verifies the level without depending on a message it may filter out.
function Set-GuiSmokeLogLevel {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [ValidateRange(0, 2)][int]$Index)
    if (-not $PSCmdlet.ShouldProcess('Owned isolated GUI smoke settings', "Apply log level $Index")) { return }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 700 -DesignY 384 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    for ($row = 0; $row -lt $Index; ++$row) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
    Assert-SettingsValue -Path $SettingsPath -Name 'logLevel' -Expected @(Get-GuiSettingsChoiceValue -Kind LogLevel)[$Index]
}

# Purpose: Reject unusable, remote and misplaced native drops without changing queued inputs; enforce exact capacity.
# Inputs: Owned normal-size Queue, one real local file, isolated log/settings and capture naming.
# Outputs: Returns captures; checks rejection logs/rendered row stability and bounded capacity, then clears Queue.
function Assert-QueueDropAdmission {
    param([IntPtr]$Handle, [int]$Dpi, [string]$InputPath, [string]$SettingsPath,
        [string]$BasePath, [string]$Extension)
    $originalLevel = Get-GuiSettingsChoiceIndex -Settings ([IO.File]::ReadAllText($SettingsPath) | ConvertFrom-Json) -Kind LogLevel
    $failure = $null
    try {
        # Successful drop admissions are Debug events; never inherit an unrelated scenario's logging state.
        Set-GuiSmokeLogLevel -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -Index 2
        $log = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($InputPath)
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Shell drop added 1 queued item(s)'
        $initial = "${BasePath}-Drop-Initial$Extension"
        $captures = @(Save-FlitzZipScreenshot -Handle $Handle -Path $initial)
        $offset = Get-ClientCaptureOffset -Handle $Handle
        $bounds = @{ Dpi = $Dpi; Offset = $offset; Left = 122; Top = 132; Right = 1140; Bottom = 240 }
        $expected = Get-GuiTextBodyDigest -Path $initial @bounds
        $invalid = 'Shell drop rejected a remote, device, relative, or malformed path'
        foreach ($case in @(@{ Name = 'Empty'; Paths = @(); Page = 0; X = 300; Message = 'Shell drop did not contain usable paths' },
                @{ Name = 'Relative'; Paths = @('relative.txt'); Page = 0; X = 300; Message = $invalid },
                @{ Name = 'Remote'; Paths = @('\\example.invalid\share\file.txt'); Page = 0; X = 300; Message = $invalid },
                @{ Name = 'Device'; Paths = @('\\?\C:\file.txt'); Page = 0; X = 300; Message = $invalid },
                @{ Name = 'Dot'; Paths = @($InputPath + '\..\file.txt'); Page = 0; X = 300; Message = $invalid },
                @{ Name = 'Outside'; Paths = @($InputPath); Page = 0; X = 43; Message = 'Shell drop rejected outside the Queue table' },
                @{ Name = 'Other-Page'; Paths = @($InputPath); Page = 1; X = 300; Message = 'Shell drop rejected outside the Queue table' })) {
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex $case.Page -Synchronous
            $length = (Get-Item -LiteralPath $log).Length
            Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths $case.Paths -DesignX $case.X
            Wait-GuiLogEvent -Path $log -PreviousLength $length -Message $case.Message
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
            $path = "${BasePath}-Drop-$($case.Name)$Extension"
            $captures += Save-FlitzZipScreenshot -Handle $Handle -Path $path
            if ((Get-GuiTextBodyDigest -Path $path @bounds) -cne $expected) {
                throw "A rejected $($case.Name) shell drop changed the rendered Queue row."
            }
        }
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths (@($InputPath) * 4096)
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Shell drop added 4096 queued item(s)' -TimeoutSeconds 15
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($InputPath)
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Shell drop rejected because the Queue is full'
        $captures += Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Drop-Capacity$Extension"
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($InputPath)
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Shell drop added 1 queued item(s)'
        $recovery = "${BasePath}-Drop-Recovery$Extension"
        $captures += Save-FlitzZipScreenshot -Handle $Handle -Path $recovery
        if ((Get-GuiTextBodyDigest -Path $recovery @bounds) -cne $expected) {
            throw 'Queue drop admission did not recover after clearing a full Queue.'
        }
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
        return $captures
    } catch { $failure = $_.Exception; throw }
    finally {
        try {
            Set-GuiSmokeLogLevel -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -Index $originalLevel
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
        } catch {
            if ($failure) { $failure.Data['DropLogSettingsRestoreFailure'] = $_.Exception.Message }
            else { throw }
        }
    }
}

# Purpose: Keep keyboard selection inside the only visible History row at both arrow boundaries.
# Inputs: Owned normal-size History page with exactly one filtered row, and capture naming.
# Outputs: Proves row focus survives Up/Down and both activation keys without clearing history or navigating away.
function Assert-HistoryKeyboardRow {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)

    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    foreach ($key in @(0x26, 0x28, 0x20, 0x0D)) { Invoke-ClientKey -Handle $Handle -VirtualKey $key }
    $path = "${BasePath}-History-Keyboard-Row$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $path
    $offset = Get-ClientCaptureOffset -Handle $Handle
    Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 130 -Top 222 -Right 300 -Bottom 226 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
    Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 1 -Top 319 -Right 4 -Bottom 361 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 8 -MinPixels 100
}

# Purpose: Keep keyboard row identity correct while scrolling past both ends of the normal Queue viewport.
# Inputs: Owned Queue containing the 72 ordered fixture paths, isolated ZIP destination and capture names.
# Outputs: Independently reads exactly the 70 selected ZIP members after deselecting rows 54 and zero by keyboard.
function Assert-QueueKeyboardScrolling {
    param([IntPtr]$Handle, [int]$Dpi, [string[]]$Paths, [string]$Destination, [string]$BasePath, [string]$Extension)

    if ($Paths.Count -ne 72) { throw 'Keyboard scrolling requires the complete ordered Queue fixture.' }
    # The fixed 1200x760 design shows 15 rows: End focuses visible row 14, then 40 Down keys reach row 54.
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    foreach ($step in 1..40) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $bottom = "${BasePath}-Queue-Keyboard-Scrolled$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $bottom
    Assert-DesignRectHasColor -Path $bottom -Dpi $Dpi -Left 130 -Top 640 -Right 300 -Bottom 644 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
    foreach ($step in 1..54) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x26 }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
    $top = "${BasePath}-Queue-Keyboard-Returned$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $top
    Assert-DesignRectHasColor -Path $top -Dpi $Dpi -Left 130 -Top 164 -Right 300 -Bottom 168 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
    $expected = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
    foreach ($index in 1..71) {
        if ($index -ne 54) { $expected.Add([IO.Path]::GetFileName($Paths[$index]), $Paths[$index]) }
    }
    [IO.File]::Delete((Join-Path $Destination 'FlitzZip-output.zip'))
    Assert-GuiCreatedZip -Handle $Handle -Dpi $Dpi -Destination $Destination -Expected $expected -BasePath "${BasePath}-Queue-Keyboard" -Extension $Extension
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
}

# Purpose: Qualify keyboard access, tab isolation, scroll recovery and each close key in the native license modal.
# Inputs: Owned normal-size About page, its settings/log path and screenshot naming.
# Outputs: Pixel-verified modal states and restored text after Home; close keys retain the About page.
function Assert-LicenseKeyboardNavigation {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $log = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $body = @{ Left = 300; Top = 250; Right = 940; Bottom = 550 }
    foreach ($closeKey in @(0x0D, 0x20, 0x1B)) {
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'License notices opened'
        if ($closeKey -eq 0x0D) {
            $initial = "${BasePath}-License-Keyboard-Initial$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $initial
            $expected = Get-GuiTextBodyDigest -Path $initial -Dpi $Dpi -Offset $offset @body
            foreach ($step in @('Other', 'Returned', 'Bottom', 'Top')) {
                $key = switch ($step) { 'Other' { 0x27 }; 'Returned' { 0x25 }; 'Bottom' { 0x23 }; 'Top' { 0x24 } }
                Invoke-ClientKey -Handle $Handle -VirtualKey $key
                if ($step -eq 'Top') {
                    # Every scroll key remains modal; finish at the independently observed original text position.
                    foreach ($scrollKey in @(0x28, 0x22, 0x21, 0x26, 0x24)) {
                        Invoke-ClientKey -Handle $Handle -VirtualKey $scrollKey
                    }
                }
                Start-Sleep -Milliseconds 220
                $path = "${BasePath}-License-Keyboard-$step$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $path
                $actual = Get-GuiTextBodyDigest -Path $path -Dpi $Dpi -Offset $offset @body
                $same = $step -in @('Returned', 'Top')
                if (($actual -ceq $expected) -ne $same) { throw "License keyboard $step lost its text/tab state." }
            }
        }
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-ClientKey -Handle $Handle -VirtualKey $closeKey
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'License notices closed'
        $path = "${BasePath}-License-Keyboard-Closed-$closeKey$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $path
        Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 1 -Top 511 -Right 4 -Bottom 553 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 8 -MinPixels 100
    }
}

# Purpose: Verify Queue divider tracking through actual normal and compact painted geometry.
# Inputs: Owned window/DPI and capture prefix; one admitted Queue row remains available to later smoke consumers.
# Outputs: Requires exact bidirectional physical movement, restores viewport/pointer, and returns to Extract.
function Assert-GuiQueueColumnResize {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)

    Invoke-GuiPointerScope -ArgumentList @($Handle, $Dpi, $BasePath, $Extension) -Action {
        param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
        foreach ($width in @(1200, 960)) {
            $height = if ($width -eq 1200) { 760 } else { 600 }
            $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width $width -Height $height
            try {
                $offset = Get-ClientCaptureOffset -Handle $Handle
                $left = if ($width -eq 1200) { 420 } else { 310 }
                $right = if ($width -eq 1200) { 510 } else { 410 }
                $prefix = "$BasePath-Queue-Column-$width"
                $before = "$prefix-Before$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $before -ExpectedDesignWidth $width -ExpectedDesignHeight $height
                $start = Get-GuiColumnSeparator -Path $before -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left $left -Right $right -Top 136
                Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($start)) -DesignY 145
                Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($start)) -StartY 145 -EndX ([int][Math]::Round($start) - 40) -EndY 145
                $after = "$prefix-Moved$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $after -ExpectedDesignWidth $width -ExpectedDesignHeight $height
                $moved = Get-GuiColumnSeparator -Path $after -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left ([int][Math]::Round($start) - 65) -Right ([int][Math]::Round($start) + 5) -Top 136
                if ([Math]::Abs($moved - $start + 40) -gt 1) { throw 'Queue divider did not follow the physical pointer distance.' }
                Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($moved)) -DesignY 145
                Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($moved)) -StartY 145 -EndX ([int][Math]::Round($start)) -EndY 145
                $restored = "$prefix-Restored$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $restored -ExpectedDesignWidth $width -ExpectedDesignHeight $height
                $end = Get-GuiColumnSeparator -Path $restored -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left $left -Right $right -Top 136
                if ([Math]::Abs($end - $start) -gt 1) { throw 'Queue reverse drag did not restore its original painted boundary.' }
            } finally { Restore-SmokeWindowSize -Handle $Handle -Snapshot $old }
        }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
    }
}

# Purpose: Reject stale drag geometry after a real captured gesture changes the viewport.
# Inputs: Owned window/DPI and capture prefix; trusted helpers keep press/release ownership bounded.
# Outputs: Proves active movement, cancellation of stale input and successful resizing after cancellation.
function Assert-GuiResizeCaptureCancellation {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)

    Invoke-GuiPointerScope -ArgumentList @($Handle, $Dpi, $BasePath, $Extension) -Action {
        param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
        $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 1200 -Height 760
        try {
            $prefix = "$BasePath-Queue-Capture"
            $offset = Get-ClientCaptureOffset -Handle $Handle
            $before = "$prefix-Before$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $before
            $start = Get-GuiColumnSeparator -Path $before -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 420 -Right 510 -Top 136
            $observation = @{}
            Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($start)) -DesignY 145
            Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ([int][Math]::Round($start)) -StartY 145 -EndX ([int][Math]::Round($start) + 80) -EndY 145 -DuringDrag {
                param([IntPtr]$Handle, [int]$Dpi, [string]$Prefix, [string]$Extension, [double]$Start, [hashtable]$Observation)
                Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX ([int][Math]::Round($Start) + 10) -DesignY 145
                $offset = Get-ClientCaptureOffset -Handle $Handle
                $active = "$Prefix-Active$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $active
                $moved = Get-GuiColumnSeparator -Path $active -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 420 -Right 520 -Top 136
                if ([Math]::Abs($moved - $Start - 10) -gt 1) { throw 'The cancellation control never established an active column drag.' }
                [void](Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600)
                $resized = "$Prefix-Viewport$Extension"
                Save-FlitzZipScreenshot -Handle $Handle -Path $resized -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
                $offset = Get-ClientCaptureOffset -Handle $Handle
                $Observation.Boundary = Get-GuiColumnSeparator -Path $resized -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 310 -Right 420 -Top 136
            } -DuringDragArgumentList @($Handle, $Dpi, $prefix, $Extension, $start, $observation)
            $released = "$prefix-Released$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $released -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
            $offset = Get-ClientCaptureOffset -Handle $Handle
            $end = Get-GuiColumnSeparator -Path $released -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 310 -Right 420 -Top 136
            if (-not $observation.ContainsKey('Boundary') -or [Math]::Abs($end - $observation.Boundary) -gt 1) {
                throw 'A stale captured move resized columns after the viewport changed.'
            }
            [void](Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 1200 -Height 760)
            $restoring = [int][Math]::Round($start) + 10
            Invoke-ClientMove -Handle $Handle -Dpi $Dpi -DesignX $restoring -DesignY 145
            Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX $restoring -StartY 145 -EndX ([int][Math]::Round($start)) -EndY 145
            $reused = "$prefix-Reused$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $reused
            $offset = Get-ClientCaptureOffset -Handle $Handle
            $end = Get-GuiColumnSeparator -Path $reused -Dpi $Dpi -OffsetX $offset.X -OffsetY $offset.Y -Left 420 -Right 510 -Top 136
            if ([Math]::Abs($end - $start) -gt 1) { throw 'Capture cancellation prevented a subsequent valid resize.' }
        } finally { Restore-SmokeWindowSize -Handle $Handle -Snapshot $old }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
    }
}

# Purpose: Verify System, Queue, and History behavior contracts from GUI source.
# Inputs: `SourceText` is the joined GUI source text.
# Outputs: Throws when a visual or behavior regression boundary is missing.
function Assert-GuiSystemQueueContract {
    param([Parameter(Mandatory = $true)][string]$SourceText)

    if ($SourceText -cmatch 'found_process_sample') {
        throw "System GPU utilization graph must not prefer process-only PDH samples."
    }
    if ($SourceText -cmatch 'vram_span') {
        throw "System GPU graph must plot total GPU utilization, not VRAM history."
    }
    if (-not $SourceText.Contains('system_gpu_utilization(samples)')) {
        throw "System GPU utilization must use the regression-tested physical-engine aggregation."
    }
    if (-not $SourceText.Contains('current_user_downloads_directory')) {
        throw "GUI destination defaults must resolve the current user's Downloads folder instead of process cwd."
    }
    if ($SourceText -match 'std::filesystem::current_path|GetCurrentDirectoryW?|safe_current_path') {
        throw "GUI destination defaults must never fall back to the process current directory."
    }
    foreach ($requiredIoSource in @(
        'DropdownId::SystemIoDrive',
        'fixed_io_drive_options',
        'GetDriveTypeW(root.c_str()) == DRIVE_FIXED',
        'sample_selected_drive_io',
        'LogicalDisk(',
        '% Disk Time',
        'Disk Read Bytes/sec',
        'Disk Write Bytes/sec',
        'io_busy_percent')) {
        if (-not $SourceText.Contains($requiredIoSource)) {
            throw "System I/O monitor must use selected fixed-drive total LogicalDisk counters; missing $requiredIoSource."
        }
    }
    if ($SourceText -cmatch 'GetProcessIoCounters|sample_process_io_rates') {
        throw "System I/O monitor must not regress to FlitzZip process-only I/O counters."
    }
    if (-not $SourceText.Contains('queue_scrollbar_thumb_rect') -or -not $SourceText.Contains('WM_MOUSEWHEEL')) {
        throw "Queue overflow must keep a fixed header and expose a working scrollbar/wheel path."
    }
    if (-not $SourceText.Contains('std::array<PerformanceMonitorSample, 96> performance_history_')) {
        throw "System graph history cadence must not be changed without an explicit graph-cadence task."
    }
    if (-not $SourceText.Contains('Remove selected')) {
        throw "Queue must expose a Remove selected action when checked rows exist."
    }
    if (-not $SourceText.Contains('history_column_resize_separator_')) {
        throw "History table must keep Queue-equivalent column resizing support."
    }
    if ($SourceText.Contains('draw_interactive_hover_surface(dc, rect, interactive);')) {
        throw "Empty checkbox hover must not paint a row-sized placeholder surface."
    }
    if (-not $SourceText.Contains('const bool has_label = text != nullptr && text[0] != L''\0'';') -or
        -not $SourceText.Contains('const RECT paint_rect = has_label ? rect : hover_rect;')) {
        throw "Checkbox hover rendering must use a tight hover surface when there is no label text."
    }
    if (-not $SourceText.Contains('const int checkbox_target_size = scale(24);')) {
        throw "Queue checkbox hit/focus targets must stay tightly centered around the visible tick."
    }
    foreach ($requiredFolderSizeSource in @(
        'queue_entry_size_text',
        'return L"..."',
        'THREAD_PRIORITY_LOWEST',
        'THREAD_MODE_BACKGROUND_BEGIN',
        'recursive_directory_iterator',
        'measure_folder_size',
        'directory_options::none'
    )) {
        if (-not $SourceText.Contains($requiredFolderSizeSource)) {
            throw "Queue folder sizes must be resolved by a bounded low-priority background metadata scan; missing $requiredFolderSizeSource."
        }
    }
}

# Purpose: Verify security, picker, and extraction contracts from GUI source.
# Inputs: `SourceText` is the joined GUI source text.
# Outputs: Throws when GUI controls are detached from real secure operations.
function Assert-GuiSecurityPickerContract {
    param([Parameter(Mandatory = $true)][string]$SourceText)

    if ($SourceText -cmatch '\.string\s*\(') {
        throw 'GUI filesystem paths must use explicit UTF-8 or native wide text, not locale-dependent path.string().'
    }
    foreach ($requiredSecurityCall in @(
        'hash_path(output, IntegrityMode::Sha256)',
        'hash_path(archive_source.path(), IntegrityMode::Sha256)',
        'hash_path(pinned.path(), IntegrityMode::Sha256)',
        'scan_with_windows_defender(output, DefenderScanMode::FullPath)',
        'scan_with_windows_defender(archive_source.path(), DefenderScanMode::FullPath)',
        'scan_with_windows_defender(pinned.path(), DefenderScanMode::FullPath)',
        'validate_detected_archive(',
        'SecurityCheckState::Passed',
        'SecurityCheckState::Incomplete'
    )) {
        if (-not $SourceText.Contains($requiredSecurityCall)) {
            throw "GUI security options must call the real integrity and Defender paths; missing $requiredSecurityCall."
        }
    }
    if ($SourceText.Contains('{L"Path safety", L"Safe"') -or
        $SourceText.Contains('{L"CRC metadata", L"Verified"')) {
        throw "Security-page result rows must not contain fixed positive claims."
    }
    if ($SourceText -cmatch 'OPENFILENAMEW|GetOpenFileNameW|SHBrowseForFolderW|SHGetPathFromIDListW') {
        throw "Queue Add files/Add folder must use the modern shell picker without fixed legacy buffers."
    }
    if (-not $SourceText.Contains('IFileOpenDialog') -or -not $SourceText.Contains('append_queued_paths')) {
        throw "Queue Add files, Add folder, and drag/drop must share modern shell selection and queue append paths."
    }
    if (-not $SourceText.Contains('detect_archive_format_by_extension(path)') -or
        -not $SourceText.Contains('return L"Archive";')) {
        throw "Queue Type must classify supported archive files as Archive through extension-only detection."
    }
    foreach ($requiredExtractSource in @(
        'selected_extract_archive_paths',
        'Multiple selected archives',
        'L"Archive path"',
        'extraction_outputs_for_archives',
        'request.archives'
    )) {
        if (-not $SourceText.Contains($requiredExtractSource)) {
            throw "Extract page must support selected one-or-many archive extraction; missing $requiredExtractSource."
        }
    }
    if (-not $SourceText.Contains('draw_extract_overwrite_prompt')) {
        throw "Extract Ask-before-overwriting policy must use a FlitzZip-owned in-app modal."
    }
    if ($SourceText -cmatch 'MessageBoxW') {
        throw "Product confirmations must not use native MessageBoxW; use FlitzZip-owned modal surfaces."
    }
}

# Purpose: Verify telemetry, format ordering, drag/drop, and license-window contracts.
# Inputs: `SourceText` is the joined GUI source text.
# Outputs: Throws when GUI policy contracts drift.
function Assert-GuiFormatTelemetryLicenseContract {
    param([Parameter(Mandatory = $true)][string]$SourceText)

    if (-not $SourceText.Contains('constexpr std::size_t kFullGraphSampleCapacity = 96U') -or
        -not $SourceText.Contains('first_x')) {
        throw "Performance graphs must not stretch startup samples across the full plot."
    }
    if (-not $SourceText.Contains('constexpr UINT kTextTooltipDelayMs = 500')) {
        throw "Truncated text tooltip delay must remain 0.5 seconds."
    }
    if (-not $SourceText.Contains('performance_update_seconds = 3')) {
        throw "System Performance Monitor default refresh interval must remain 3 seconds."
    }
    if (-not $SourceText.Contains('".flzip",   ".zip"') -or
        -not $SourceText.Contains('constexpr int kDefaultCompressionFormatIndex = 0')) {
        throw "Compress Format must list .flzip first and use it as the default row."
    }
    if ($SourceText.Contains('DragAcceptFiles(hwnd_, TRUE)')) {
        throw "GUI drag/drop must not advertise the full window as a shell drop surface; use the Queue OLE drop target."
    }
    if ($SourceText.Contains('WS_EX_ACCEPTFILES')) {
        throw "GUI drag/drop must not set WS_EX_ACCEPTFILES on the whole window; OLE hit testing controls the allowed drop surface."
    }
    if ($SourceText.Contains('ChangeWindowMessageFilterEx')) {
        throw "GUI drag/drop must preserve UIPI defaults for elevated windows."
    }
    foreach ($requiredDropBoundary in @(
        'kMaxShellDropPayloadBytes',
        'kMaxQueueItems',
        'is_supported_local_drop_path',
        'kMaxFolderSizeEntries',
        'kMaxFolderSizeDuration'
    )) {
        if (-not $SourceText.Contains($requiredDropBoundary)) {
            throw "Queue drag/drop and folder metadata work must remain local-only and bounded; missing $requiredDropBoundary."
        }
    }
    if (-not $SourceText.Contains('is_copy_accelerator')) {
        throw "FlitzZip-owned UI must consume text-copy accelerators instead of exposing copyable text."
    }
    if ($SourceText -match 'OpenClipboard|SetClipboardData|GetClipboardData|CreateWindowExW[^\r\n]*(EDIT|RICHEDIT)|CreateWindowW[^\r\n]*(EDIT|RICHEDIT)') {
        throw "FlitzZip-owned UI must not expose selectable text controls or clipboard APIs."
    }
    if (-not $SourceText.Contains('license_notices_dialog_visible') -or
        -not $SourceText.Contains('flitzzip_license_notices.hpp')) {
        throw "About Licenses must be backed by the generated license-notices header and modal state."
    }
    if (-not $SourceText.Contains('license_notices_tab_rects') -or
        -not $SourceText.Contains('select_license_notices_tab')) {
        throw "About Licenses must keep FlitzZip and Other tabs for readable non-copyable notices."
    }
    foreach ($requiredSmoothScrollSource in @(
        'DeferredMouseCommand::ShowLicenseNotices',
        'DeferredMouseCommand::CloseLicenseNotices',
        'release_deferred_mouse_command',
        'kDeferredCommandTimer',
        'set_license_notices_scroll_target',
        'license_notices_scroll_animation_start_',
        'set_history_details_scroll_target',
        'history_details_scroll_animation_start_',
        'tick_smooth_scroll_animation',
        'kSmoothScrollTransitionMs')) {
        if (-not $SourceText.Contains($requiredSmoothScrollSource)) {
            throw "Licenses and History details must use deferred button release plus bounded smooth scrolling; missing $requiredSmoothScrollSource."
        }
    }
    if ($SourceText -cmatch 'queue_scroll_target|history_scroll_target') {
        throw "Only Licenses and History details may use smooth pixel-scroll targets; Queue and History tables must stay row-based."
    }
    $modalAccentLine = 'RECT{rect.left, workspace.top, rect.right, workspace.top + scale(2)}'
    if ([regex]::Matches($SourceText, [regex]::Escape($modalAccentLine)).Count -lt 2) {
        throw "FlitzZip-owned secondary windows must use the same full-width top red line as the main interface."
    }
    if (-not $SourceText.Contains('page_title_rect(area)')) {
        throw "Top-level page titles must share the button-aligned page_title_rect geometry."
    }
    if (-not $SourceText.Contains('i == 1 ? L"GPU"') -or
        -not $SourceText.Contains('i == 2 ? L"RAM"') -or
        -not $SourceText.Contains('const RECT io_card = cards[3]')) {
        throw "System graphs must render in CPU, GPU, RAM, I/O order with the I/O drive selector on the I/O card."
    }
    if (-not $SourceText.Contains('sample_total_dedicated_vram_used_bytes') -or
        -not $SourceText.Contains('selected_gpu_memory_usage(samples, *adapter_luid, process_id)') -or
        -not $SourceText.Contains('cached_gpu_adapter_luid_ = info.adapter_luid') -or
        -not $SourceText.Contains('reconcile_vram_usage') -or
        -not (Get-Content -Raw -LiteralPath (Join-Path $repo 'tests/cpp/test_resource_usage.cpp')).Contains('vram_reconciliation_keeps_process_usage_under_total_usage') -or
        -not (Get-Content -Raw -LiteralPath (Join-Path $repo 'tests/cpp/test_resource_usage.cpp')).Contains('selected_gpu_memory_matches_adapter_and_process_identity')) {
        throw "VRAM total/dedicated display must match the selected HIP adapter identity and use tested reconciliation."
    }
}

# Purpose: Run every GUI source-contract assertion as a fast pre-smoke gate.
# Inputs: None; reads the repository app source tree.
# Outputs: Throws with a precise remediation message when a GUI contract is violated.
function Assert-GuiSourceContract {
    $sourceText = Get-GuiSourceText
    Assert-GuiBlockedText -SourceText $sourceText
    Assert-GuiSystemQueueContract -SourceText $sourceText
    Assert-GuiSecurityPickerContract -SourceText $sourceText
    Assert-GuiFormatTelemetryLicenseContract -SourceText $sourceText
}

Assert-GuiSourceContract

# Purpose: Select a Compress format row through the same keyboard path users can use.
# Inputs: `Handle`/`Dpi` identify the FlitzZip window and `Index` is the zero-based Compress format row.
# Outputs: Opens the dropdown, moves to the requested row, selects it, and waits for repaint.
function Select-CompressFormatIndex {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [int]$Index
    )
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 224 -Synchronous
    Start-Sleep -Milliseconds 120
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    for ($i = 0; $i -lt $Index; ++$i) {
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
        Start-Sleep -Milliseconds 15
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Start-Sleep -Milliseconds 180
}

$smokeSource = Get-Content -Raw -LiteralPath $PSCommandPath
if (-not $smokeSource.Contains('Queue-AfterBulkDragDrop')) {
    throw "GUI smoke must exercise a many-file Queue drag/drop payload."
}
foreach ($requiredSmokeExercise in @('About-Licenses-SmoothWheel', 'History-Details-SmoothWheel', 'Invoke-ClientWheel')) {
    if (-not $smokeSource.Contains($requiredSmokeExercise)) {
        throw "GUI smoke must exercise smooth mouse-wheel scrolling for Licenses and History details; missing $requiredSmokeExercise."
    }
}

Import-Module (Join-Path $PSScriptRoot "FlitzZip.GuiSmoke.Ui.psm1") -Force
Import-Module (Join-Path $PSScriptRoot "FlitzZip.GuiSmoke.Pickers.psm1") -Force
Import-Module (Join-Path $PSScriptRoot "FlitzZip.GuiSmoke.Hover.psm1") -Force

# Purpose: Check sparse-detail detection independently of any product window or host font/DPI.
# Inputs: None; creates one owned temporary image.
# Outputs: Requires rejection of a flat image and detection of five colors missed by the coarse sampling grid.
function Assert-VisualDetailSampler {
    $path = Join-Path ([IO.Path]::GetTempPath()) ("FlitzZip-visual-detail-{0}.png" -f [guid]::NewGuid().ToString('N'))
    $bitmap = [Drawing.Bitmap]::new(240, 160)
    try {
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        $rejected = $false
        try { Assert-DesignRectHasDetail -Path $path -Dpi 96 -Left 0 -Top 0 -Right 240 -Bottom 160 }
        catch {
            if ($_.Exception.Message -notlike 'Expected rendered dropdown/detail region*') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw 'Visual detail sampler accepted a flat image.' }
        $colors = @([Drawing.Color]::Red, [Drawing.Color]::Green, [Drawing.Color]::Blue, [Drawing.Color]::White)
        for ($i = 0; $i -lt $colors.Count; ++$i) { $bitmap.SetPixel($i + 1, 1, $colors[$i]) }
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        Assert-DesignRectHasDetail -Path $path -Dpi 96 -Left 0 -Top 0 -Right 240 -Bottom 160
    } finally {
        $bitmap.Dispose()
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    Write-Output 'Visual detail sampler flat-image and sparse-detail regressions passed.'
}

Assert-VisualDetailSampler

# Purpose: Assert one persisted GUI setting value in the temporary smoke settings file.
# Inputs: `Path` is the JSON settings file, `Name` is the property, and `Expected` is the required value.
# Outputs: Throws when Apply did not persist the expected value.
function Assert-SettingsValue {
    param(
        [string]$Path,
        [string]$Name,
        [object]$Expected
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Expected settings file was not written: $Path"
    }
    $settings = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    $actual = $settings.$Name
    if ($actual -ne $Expected) {
        throw "Expected settings $Name to be '$Expected', got '$actual'."
    }
}

# Purpose: Wait for a newly appended GUI command result rather than assuming input was processed.
# Inputs: Path is the smoke log, PreviousLength is its prior byte length, Message is expected row text, and
# TimeoutSeconds is the explicit bounded wait for the selected operation.
# Outputs: Returns after the expected new row, or throws after the selected timeout without it.
function Wait-GuiLogEvent {
    param(
        [string]$Path,
        [long]$PreviousLength,
        [string]$Message,
        [ValidateRange(1, 300)][int]$TimeoutSeconds = 5
    )

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        if ((Get-Item -LiteralPath $Path).Length -gt $PreviousLength) {
            $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            try {
                $stream.Position = $PreviousLength
                $reader = [System.IO.StreamReader]::new($stream)
                try { $newRows = $reader.ReadToEnd() } finally { $reader.Dispose() }
            } finally { $stream.Dispose() }
            if ($newRows.Contains($Message)) { return }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "GUI command did not report '$Message' within $TimeoutSeconds seconds."
}

# Purpose: Verify Unicode archive paths through real Security workers, hashing, failures and subsequent reuse.
# Inputs: Owned GUI/settings, bounded valid archive, isolated fixture root and capture paths.
# Outputs: Returns captures; requires exact completion outcomes and immutable inputs, then restores policy settings.
function Assert-UnicodeSecurityReview {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$FixtureArchive,
        [string]$FixtureRoot, [string]$BasePath, [string]$Extension)
    $originalLogLevel = Get-GuiSettingsChoiceIndex -Settings (Get-Content -Raw -LiteralPath $SettingsPath | ConvertFrom-Json) -Kind LogLevel
    $original = $null
    $failure = $null
    $points = @()
    $captures = @()
    $root = Join-Path $FixtureRoot 'security-unicode'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $name = 'archive-' + [char]::ConvertFromUtf32(0x1F9EA)
    $first = Join-Path $root "$name.flzip"
    $second = Join-Path $root "$name-two.flzip"
    $invalid = Join-Path $root "$name-invalid.flzip"
    Copy-Item -LiteralPath $FixtureArchive -Destination $first -Force
    Copy-Item -LiteralPath $FixtureArchive -Destination $second -Force
    [IO.File]::WriteAllText($invalid, 'Independent malformed archive')
    $before = @(Get-ChildItem -LiteralPath $root -File | Get-FileHash | Sort-Object Path |
        ForEach-Object { "$($_.Path)=$($_.Hash)" }) -join "`n"
    $log = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    try {
        Set-GuiSmokeLogLevel -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -Index 2
        # Applying Settings persists pending Compress choices; snapshot the live policy after that boundary.
        $original = Get-Content -Raw -LiteralPath $SettingsPath | ConvertFrom-Json
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
        foreach ($choice in @(@{ Name = 'integrityHashOptIn'; Expected = $true; Y = 376 },
                @{ Name = 'defenderScanOptIn'; Expected = $false; Y = 412 },
                @{ Name = 'gpuRequired'; Expected = $false; Y = 448 },
                @{ Name = 'showOperationSummary'; Expected = $false; Y = 227 })) {
            if ($original.($choice.Name) -ne $choice.Expected) {
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY $choice.Y -Synchronous
                $points += $choice.Y
            }
        }
        Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
        Assert-SettingsValue -Path $SettingsPath -Name 'integrityHashOptIn' -Expected $true
        Assert-SettingsValue -Path $SettingsPath -Name 'defenderScanOptIn' -Expected $false
        Assert-SettingsValue -Path $SettingsPath -Name 'gpuRequired' -Expected $false
        Assert-SettingsValue -Path $SettingsPath -Name 'showOperationSummary' -Expected $false
        foreach ($case in @(@{ Name = 'Single'; Paths = @($first); Result = 'completed'; HashComplete = $true },
                @{ Name = 'Malformed'; Paths = @($invalid); Result = 'failed'; HashComplete = $true },
                @{ Name = 'Partial'; Paths = @($invalid, $first); Result = 'failed'; HashComplete = $false },
                @{ Name = 'Multiple-Recovery'; Paths = @($first, $second); Result = 'completed'; HashComplete = $true })) {
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
            $length = (Get-Item -LiteralPath $log).Length
            Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths $case.Paths
            Wait-GuiLogEvent -Path $log -PreviousLength $length -Message "Shell drop added $($case.Paths.Count) queued item(s)"
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 3 -Synchronous
            $length = (Get-Item -LiteralPath $log).Length
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
            Wait-GuiLogEvent -Path $log -PreviousLength $length -Message "Security: $($case.Result)" -TimeoutSeconds 15
            $capture = "${BasePath}-Security-Unicode-$($case.Name)$Extension"
            $captures += Save-FlitzZipScreenshot -Handle $Handle -Path $capture
            $offset = Get-ClientCaptureOffset -Handle $Handle
            $hashRgb = if ($case.HashComplete) { @(83, 210, 101) } else { @(237, 179, 61) }
            Assert-DesignRectHasColor -Path $capture -Dpi $Dpi -Left 320 -Top 246 -Right 510 -Bottom 274 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed $hashRgb[0] -ExpectedGreen $hashRgb[1] -ExpectedBlue $hashRgb[2] -Tolerance 20 -MinPixels 4
            foreach ($top in @(170, 208)) {
                $rgb = if ($case.Result -eq 'completed') { @(83, 210, 101) } else { @(236, 73, 73) }
                Assert-DesignRectHasColor -Path $capture -Dpi $Dpi -Left 320 -Top $top -Right 510 -Bottom ($top + 28) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed $rgb[0] -ExpectedGreen $rgb[1] -ExpectedBlue $rgb[2] -Tolerance 20 -MinPixels 4
            }
        }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4 -Synchronous
        $captures += Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Security-Unicode-History$Extension"
        $after = @(Get-ChildItem -LiteralPath $root -File | Get-FileHash | Sort-Object Path |
            ForEach-Object { "$($_.Path)=$($_.Hash)" }) -join "`n"
        if ($before -cne $after -or @(Get-ChildItem -LiteralPath $root).Count -ne 3) {
            throw 'Security review changed its independent input bytes or published unexpected output.'
        }
    } catch { $failure = $_.Exception; throw }
    finally {
        try {
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
            foreach ($y in $points) {
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY $y -Synchronous
            }
            Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
            if ($original) {
                foreach ($name in @('integrityHashOptIn', 'defenderScanOptIn', 'gpuRequired', 'showOperationSummary')) {
                    Assert-SettingsValue -Path $SettingsPath -Name $name -Expected $original.$name
                }
            }
            Set-GuiSmokeLogLevel -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -Index $originalLogLevel
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        } catch {
            if ($failure) { $failure.Data['SecuritySettingsRestoreFailure'] = $_.Exception.Message }
            else { throw }
        }
    }
    return $captures
}

# Purpose: Verify selected inputs through a GUI-created ZIP and an independent .NET reader.
# Inputs: Handle/Dpi identify the owned GUI; Destination and Expected contain only smoke-owned paths; BasePath/Extension locate the capture.
# Outputs: Requires exactly the selected files and hashes, rejects cancelled/parent paths, and captures the result.
function Assert-GuiCreatedZip {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Destination, [Collections.Generic.Dictionary[string, string]]$Expected, [string]$BasePath, [string]$Extension)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
    Select-CompressFormatIndex -Handle $Handle -Dpi $Dpi -Index 1
    $logPath = Join-Path (Join-Path ([IO.Path]::GetTempPath()) 'FlitzZip') 'flitzzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Compress: completed' -TimeoutSeconds 15
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $Destination 'FlitzZip-output.zip'))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $files = @($archive.Entries | Where-Object { $_.Name.Length -gt 0 })
        if ($files.Count -ne $Expected.Count -or $archive.Entries.Count -gt $Expected.Count + 1) {
            throw 'GUI-created ZIP contains unexpected or missing entries.'
        }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($entry in $files) {
            if (-not $Expected.ContainsKey($entry.FullName) -or -not $seen.Add($entry.FullName)) {
                throw 'GUI-created ZIP has an unexpected or duplicate file path.'
            }
            $source = $Expected[$entry.FullName]
            if ($entry.Length -ne (Get-Item -LiteralPath $source).Length) { throw 'GUI-created ZIP changed a selected file length.' }
            $stream = $entry.Open()
            try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') } finally { $stream.Dispose() }
            if ($actual -cne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) { throw 'GUI-created ZIP changed selected file bytes.' }
        }
        foreach ($entry in $archive.Entries) {
            if ($entry.Name.Length -eq 0 -and -not @($Expected.Keys | Where-Object { $_.StartsWith($entry.FullName, [StringComparison]::Ordinal) }).Count) {
                throw 'GUI-created ZIP has an unexpected directory.'
            }
        }
    } finally { $sha.Dispose(); $archive.Dispose() }
    Write-Information "GUI-created ZIP has exactly $($Expected.Count) selected files and byte-exact independent readback." -InformationAction Continue
    Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-ArchiveReadback$Extension"
}

# Purpose: Prove mixed Queue removal preserves unchecked paths and their unchecked state through later commands.
# Inputs: Owned GUI/DPI, four distinct smoke files, isolated destination and screenshot prefix.
# Outputs: Captures retained/unchanged/reselected rows and independently verifies the exact two retained ZIP members.
function Assert-MixedQueueRemoval {
    param([IntPtr]$Handle, [int]$Dpi, [string[]]$Paths, [string]$Destination, [string]$BasePath, [string]$Extension)

    if ($Paths.Count -ne 4) { throw 'Mixed Queue removal requires exactly four fixture files.' }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
    Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths $Paths
    # Keyboard movement must keep focus on the selected row, not activate sidebar navigation.
    $offset = Get-ClientCaptureOffset -Handle $Handle
    $ready = "${BasePath}-Queue-Mixed-Keyboard-Ready$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $ready
    foreach ($y in @(182, 216, 250, 284)) {
        Assert-DesignRectHasColor -Path $ready -Dpi $Dpi -Left 130 -Top ($y - 8) -Right 146 -Bottom ($y + 8) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    foreach ($step in 1..2) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x26 }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
    $keyboard = "${BasePath}-Queue-Mixed-Keyboard-Row1$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $keyboard
    Assert-DesignRectHasColor -Path $keyboard -Dpi $Dpi -Left 130 -Top 199 -Right 300 -Bottom 203 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
    Assert-DesignRectHasColor -Path $keyboard -Dpi $Dpi -Left 130 -Top 208 -Right 146 -Bottom 224 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 54 -ExpectedGreen 72 -ExpectedBlue 78 -Tolerance 2 -MinPixels 8
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
    $moved = "${BasePath}-Queue-Mixed-Keyboard-Row2$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $moved
    Assert-DesignRectHasColor -Path $moved -Dpi $Dpi -Left 130 -Top 233 -Right 300 -Bottom 237 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 12
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x26
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x20
    foreach ($y in @(216, 284)) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 138 -DesignY $y -Synchronous
    }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 474 -DesignY 91 -Synchronous
    $offset = Get-ClientCaptureOffset -Handle $Handle
    foreach ($state in @('Retained', 'NoSelection')) {
        if ($state -eq 'NoSelection') {
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 474 -DesignY 91 -Synchronous
        }
        $path = "${BasePath}-Queue-Mixed-$state$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $path
        foreach ($y in @(146, 182, 216)) {
            Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 130 -Top ($y - 8) -Right 146 -Bottom ($y + 8) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 54 -ExpectedGreen 72 -ExpectedBlue 78 -Tolerance 2 -MinPixels 8
        }
    }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 138 -DesignY 146 -Synchronous
    $selected = "${BasePath}-Queue-Mixed-Reselected$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $selected
    foreach ($y in @(146, 182, 216)) {
        Assert-DesignRectHasColor -Path $selected -Dpi $Dpi -Left 130 -Top ($y - 8) -Right 146 -Bottom ($y + 8) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 28 -ExpectedBlue 52 -Tolerance 20 -MinPixels 20
    }
    $expected = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
    foreach ($index in @(1, 3)) { $expected.Add([IO.Path]::GetFileName($Paths[$index]), $Paths[$index]) }
    [IO.File]::Delete((Join-Path $Destination 'FlitzZip-output.zip'))
    Assert-GuiCreatedZip -Handle $Handle -Dpi $Dpi -Destination $Destination -Expected $expected -BasePath "${BasePath}-Queue-Mixed" -Extension $Extension
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
}

# Purpose: Verify legacy preference/effort migration, obsolete no-op removal and current-key precedence through actual GUI loads and Apply.
# Inputs: Exe is the built GUI; SettingsPath is the fixed redirected smoke-only settings file.
# Outputs: Checks independent launches, cleans only owned processes/settings, and throws on incorrect persistence.
function Assert-PublicationSettingsMigration {
    param([string]$Exe, [string]$SettingsPath)

    $cases = @(
        @{ Values = @{ verifyMetadataBeforeExtract = $false }; Expected = $false },
        @{ Values = @{ verifyMetadataBeforeExtract = $true }; Expected = $true },
        @{ Values = @{ verifyMetadataBeforeExtract = $true; validateBeforePublish = $false }; Expected = $false },
        @{ Values = @{ verifyMetadataBeforeExtract = $false; validateBeforePublish = $true }; Expected = $true }
    )
    foreach ($legacyIndex in 0..4) {
        $cases += @{ Values = @{ compressionLevelIndex = $legacyIndex }; Expected = $true; Effort = 2 * $legacyIndex + 1 }
    }
    foreach ($effort in 1..9) {
        $cases += @{ Values = @{ schema = 'flitzzip.settings.v3'; compressionLevel = $effort; compressionLevelIndex = 4 }; Expected = $true; Effort = $effort }
    }
    foreach ($invalid in @($null, 'invalid', 1.5, 2147483648)) {
        $cases += @{ Values = @{ compressionLevel = $invalid }; Expected = $true; Effort = 5 }
    }
    $cases += @{ Values = @{ compressionLevel = 0 }; Expected = $true; Effort = 1 }
    $cases += @{ Values = @{ compressionLevel = 10 }; Expected = $true; Effort = 9 }
    foreach ($value in @($false, $true)) {
        $cases += @{ Values = @{ solidArchive = $value; storeTimestamps = $value; deleteAfterCompression = $value;
                confirmBeforeDeleting = $value; compressionLevel = 7 }; Expected = $true; Effort = 7 }
    }
    foreach ($index in 0..2) {
        $cases += @{ Values = @{ memoryPolicyIndex = $index; compressionLevel = 7 }; Expected = $true; Effort = 7 }
    }
    foreach ($case in $cases) {
        $document = $case.Values.Clone()
        if (-not $document.ContainsKey('schema')) { $document.schema = 'flitzzip.settings.v2' }
        [IO.File]::WriteAllText($SettingsPath, ($document | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        $owned = Start-Process -FilePath $Exe -PassThru
        try {
            $deadline = (Get-Date).AddSeconds(15)
            do {
                Start-Sleep -Milliseconds 150
                $owned.Refresh()
                if ($owned.HasExited) { throw 'Settings migration GUI exited before loading.' }
            } while ($owned.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline)
            $handle = [IntPtr]$owned.MainWindowHandle
            if ($handle -eq [IntPtr]::Zero) { throw 'Settings migration GUI did not show a window.' }
            $dpi = [int][FlitzZipNativeUi]::GetDpiForWindow($handle)
            if ($dpi -le 0) { throw 'Settings migration GUI DPI is unavailable.' }
            [void](Invoke-SmokeClientResize -Handle $handle -Dpi $dpi -Width 1200 -Height 760)
            Invoke-SidebarClick -Handle $handle -Dpi $dpi -PageIndex 6 -Synchronous
            $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
            $length = (Get-Item -LiteralPath $logPath).Length
            Invoke-ClientClick -Handle $handle -Dpi $dpi -DesignX 1110 -DesignY 666 -Synchronous
            Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
            Assert-SettingsValue -Path $SettingsPath -Name 'validateBeforePublish' -Expected $case.Expected
            $expectedEffort = if ($case.ContainsKey('Effort')) { $case.Effort } else { 5 }
            Assert-SettingsValue -Path $SettingsPath -Name 'compressionLevel' -Expected $expectedEffort
            Assert-SettingsValue -Path $SettingsPath -Name 'schema' -Expected 'flitzzip.settings.v4'
            # Retired/unknown values are retained as opaque data, never interpreted as current preferences.
            foreach ($key in $case.Values.Keys) {
                if ($key -in @('solidArchive', 'storeTimestamps', 'deleteAfterCompression', 'confirmBeforeDeleting', 'memoryPolicyIndex')) {
                    Assert-SettingsValue -Path $SettingsPath -Name $key -Expected $case.Values[$key]
                }
            }
        } finally {
            if (-not $owned.HasExited) {
                [void]$owned.CloseMainWindow()
                if (-not $owned.WaitForExit(10000)) {
                    $owned.Kill()
                    if (-not $owned.WaitForExit(5000)) { throw 'Owned settings migration GUI did not exit.' }
                }
            }
            $owned.Dispose()
        }
    }
    Remove-Item -LiteralPath $SettingsPath -Force
    Write-Output "Settings migration passed for $($cases.Count) publication, legacy effort, all-nine effort, and malformed-value cases."
}

# Purpose: Prove every effort is selectable through production keyboard routing and persists as its numeric value.
# Inputs: Handle/Dpi identify the owned GUI; SettingsPath is the redirected smoke-only settings file.
# Outputs: Verifies efforts 1-9, leaves effort 5 selected, and throws if a row is missing or mapped incorrectly.
function Assert-CompressionEffortSelection {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    foreach ($effort in @(1..9) + @(5)) {
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 820 -DesignY 224 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($row = 1; $row -lt $effort; ++$row) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
        Assert-SettingsValue -Path $SettingsPath -Name 'compressionLevel' -Expected $effort
    }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
    Write-Output 'All nine compression effort selections and persistence passed.'
}

# Purpose: Persist a compression smoke choice through the real draft/apply route.
# Inputs: Owned GUI handle/DPI and its fixed redirected settings file.
# Outputs: Waits for the successful Apply event before returning to Compress.
function Save-CompressionSmokeChoice {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath)
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
}

# Purpose: Remove only an owned smoke directory after checking containment and reparse-point ancestors.
# Inputs: A strict descendant of this script's smoke root, never the smoke root itself.
# Outputs: Removes generated data or throws on unsafe paths and cleanup failures before recursive deletion.
function Remove-GuiSmokeDirectory {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true)][string]$Path)
    $root = [IO.Path]::GetFullPath($smokeRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $target = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'GUI cleanup target is not a strict descendant of its owned smoke root.'
    }
    $ancestor = $target
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'GUI cleanup refuses reparse-point ancestry.'
            }
        }
        $parent = [IO.Directory]::GetParent($ancestor)
        $ancestor = if ($parent) { $parent.FullName } else { $null }
    }
    if ((Test-Path -LiteralPath $target) -and $PSCmdlet.ShouldProcess($target, 'Remove owned GUI smoke directory')) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}

# Purpose: Create a bounded printable motif that exercises secondary GPU encoding in the real GUI worker.
# Inputs: The owned correctness-fixture path; no external corpus or performance claim is involved.
# Outputs: Writes exactly 64 KiB of deterministic correctness data without invoking native build or benchmark tools.
function Write-NeutronGuiFixture {
    param([Parameter(Mandatory = $true)][string]$Path)
    $motif = New-Object byte[] 16384
    [uint32]$state = 0x37A84E91
    for ($index = 0; $index -lt $motif.Length; ++$index) {
        $state = [uint32](($state -bxor ($state -shl 13)) -band 0xFFFFFFFFL)
        $state = [uint32](($state -bxor ($state -shr 17)) -band 0xFFFFFFFFL)
        $state = [uint32](($state -bxor ($state -shl 5)) -band 0xFFFFFFFFL)
        $motif[$index] = [byte](0x41 + ($state -band 15))
    }
    $bytes = New-Object byte[] 65536
    for ($index = 0; $index -lt $bytes.Length; ++$index) { $bytes[$index] = $motif[$index % $motif.Length] }
    [IO.File]::WriteAllBytes($Path, $bytes)
}

# Purpose: Verify a GUI-created Neutron archive with independent CPU validation and required-HIP extraction.
# Inputs: Owned GUI, its redirected log, one bounded queued input and its isolated destination.
# Outputs: Requires actual mode reporting, GPU decode telemetry and byte-exact recovery; preserves failed evidence.
function Assert-NeutronArchiveOperation {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$InputPath, [string]$Destination)
    $inputBytes = (Get-Item -LiteralPath $InputPath).Length
    if ($inputBytes -le 0 -or $inputBytes -gt 65536) { throw 'Neutron GUI correctness input exceeds its smoke bound.' }
    $archive = Join-Path $Destination 'FlitzZip-output.flzip'
    if (Test-Path -LiteralPath $archive) { throw 'Neutron GUI smoke requires an unused owned archive path.' }
    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    $lineCount = @(Get-Content -LiteralPath $logPath).Count
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Compress: completed' -TimeoutSeconds 30
    $rows = @(Get-Content -LiteralPath $logPath -Encoding UTF8 | Select-Object -Skip $lineCount)
    if (-not ($rows | Where-Object { $_.Contains('(Neutron star mode)') })) {
        throw 'The GUI worker did not report its captured Neutron compression mode.'
    }
    if (-not (Test-Path -LiteralPath $archive) -or (Get-Item -LiteralPath $archive).Length -gt 131072) {
        throw 'The GUI-created Neutron archive is missing or exceeds its smoke bound.'
    }
    $archiveBytes = [IO.File]::ReadAllBytes($archive)
    if ($archiveBytes.Length -lt 24 -or [BitConverter]::ToUInt32($archiveBytes, $archiveBytes.Length - 20) -notin @(9, 10, 11)) {
        throw 'The GUI correctness input must exercise actual Neutron compound, byte-plane or context framing.'
    }
    $restoredRoot = Join-Path $Destination ('neutron-readback-' + [guid]::NewGuid().ToString('N'))
    $cli = Join-Path $repo "build/$Configuration/flitzzip_cli.exe"
    foreach ($invocation in @(
        @{ Name = 'CPU verification'; Arguments = "verify --force-cpu `"$archive`"" },
        @{ Name = 'HIP extraction'; Arguments = "extract --require-gpu --output `"$restoredRoot`" `"$archive`"" }
    )) {
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $cli
        # These paths end in fixed owned filenames or a GUID; Windows file paths cannot contain quotes.
        $start.Arguments = $invocation.Arguments
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $child = [System.Diagnostics.Process]::Start($start)
        try {
            $stdout = $child.StandardOutput.ReadToEndAsync()
            $stderr = $child.StandardError.ReadToEndAsync()
            if (-not $child.WaitForExit(30000)) {
                $child.Kill()
                [void]$child.WaitForExit(5000)
                throw "Neutron GUI $($invocation.Name) exceeded its bounded deadline."
            }
            $output = $stdout.GetAwaiter().GetResult()
            $errorText = $stderr.GetAwaiter().GetResult()
            if ($child.ExitCode -ne 0) { throw "Neutron GUI $($invocation.Name) failed: $errorText" }
            if ($output -notmatch '(?:^|\s)entries=1(?:\s|$)') { throw 'Neutron GUI archive must contain exactly its queued input.' }
            if ($invocation.Name -eq 'HIP extraction' -and
                ($output -notmatch 'gpu_used=true' -or $output -notmatch 'gpu_kernel_launches=[1-9][0-9]*')) {
                throw 'Neutron GUI readback did not execute actual HIP kernels.'
            }
        } finally {
            $child.Dispose()
        }
    }
    $files = @(Get-ChildItem -LiteralPath $restoredRoot -File -Recurse)
    if ($files.Count -ne 1 -or $files[0].Name -cne (Split-Path -Leaf $InputPath) -or
        (Get-FileHash -LiteralPath $files[0].FullName).Hash -ne (Get-FileHash -LiteralPath $InputPath).Hash) {
        throw 'GUI-created Neutron archive did not restore its exact original bytes.'
    }
    Remove-Item -LiteralPath $files[0].FullName -Force
    Remove-Item -LiteralPath $restoredRoot -Force
    Write-Output "Neutron GUI compression, independent CPU verification and actual HIP readback passed; source_bytes=$inputBytes."
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
}

# Purpose: Exercise Neutron's separate row, real operation, persisted intent and format/policy normalization.
# Inputs: Owned GUI/settings, actual HIP capability, bounded input/destination and mutable current-run Captures.
# Outputs: Requires selection/readback, appends its row capture to Captures, and restores the applied choices.
function Assert-NeutronSelectionIsolation {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension,
        [string]$InputPath, [string]$Destination, [Parameter(Mandatory = $true)][ref]$Captures)
    $original = Get-Content -Raw -LiteralPath $SettingsPath | ConvertFrom-Json
    $info = & (Join-Path $repo "build/$Configuration/flitzzip_cli.exe") gpu-info
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect actual HIP capability for Neutron GUI qualification.' }
    $available = @($info | Where-Object { $_ -ceq 'available=true' }).Count -eq 1
    Select-CompressFormatIndex -Handle $Handle -Dpi $Dpi -Index 0
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 294 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 820 -DesignY 224 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    if ($available) {
        $Captures.Value += Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Neutron-Selection$Extension"
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
    Assert-SettingsValue -Path $SettingsPath -Name 'compressionLevel' -Expected 9
    Assert-SettingsValue -Path $SettingsPath -Name 'neutronStarMode' -Expected $available
    if ($available) {
        Assert-NeutronArchiveOperation -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -InputPath $InputPath -Destination $Destination
    }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 294 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
    Assert-SettingsValue -Path $SettingsPath -Name 'neutronStarMode' -Expected $false
    Assert-SettingsValue -Path $SettingsPath -Name 'compressionLevel' -Expected 9
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 294 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 820 -DesignY 224 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Select-CompressFormatIndex -Handle $Handle -Dpi $Dpi -Index 1
    Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
    Assert-SettingsValue -Path $SettingsPath -Name 'neutronStarMode' -Expected $false
    Assert-SettingsValue -Path $SettingsPath -Name 'compressionLevel' -Expected 9
    Select-CompressFormatIndex -Handle $Handle -Dpi $Dpi -Index (Get-GuiSettingsChoiceIndex -Settings $original -Kind Format)
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 294 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey $(if ($original.gpuRequired) { 0x24 } else { 0x23 })
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 820 -DesignY 224 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
    for ($row = 1; $row -lt $original.compressionLevel; ++$row) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    Save-CompressionSmokeChoice -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath
    Assert-SettingsValue -Path $SettingsPath -Name 'neutronStarMode' -Expected $false
    Write-Output "Neutron GUI row and policy/format isolation passed; actual_hip_available=$available."
}

# Purpose: Verify the applied summary preference changes completion navigation, including a locked-output failure.
# Inputs: Handle/Dpi identify the owned window; InputPath and Destination are isolated smoke data; SettingsPath is redirected.
# Outputs: Captures enabled/disabled/failure results, checks output and navigation, then leaves the queue empty and summary off.
function Assert-OperationSummary {
    param([IntPtr]$Handle, [int]$Dpi, [string]$InputPath, [string]$Destination,
        [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $output = Join-Path $Destination 'FlitzZip-output.zst'
    Assert-SettingsValue -Path $SettingsPath -Name 'showOperationSummary' -Expected $true
    Assert-SettingsValue -Path $SettingsPath -Name 'openDestinationAfterOperation' -Expected $false
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 194 -Synchronous
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
    Assert-SettingsValue -Path $SettingsPath -Name 'openDestinationAfterOperation' -Expected $true
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($InputPath)
    Start-Sleep -Milliseconds 150
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 820 -DesignY 154 -Synchronous
    Select-CompressFormatIndex -Handle $Handle -Dpi $Dpi -Index 6
    foreach ($scenario in @('Enabled', 'Disabled', 'Failure')) {
        if ($scenario -ne 'Enabled') {
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 194 -Synchronous
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
            $length = (Get-Item -LiteralPath $logPath).Length
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
            Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
            Assert-SettingsValue -Path $SettingsPath -Name 'showOperationSummary' -Expected ($scenario -eq 'Failure')
            Assert-SettingsValue -Path $SettingsPath -Name 'openDestinationAfterOperation' -Expected ($scenario -eq 'Failure')
            if ($scenario -eq 'Disabled') {
                # An unapplied enable must not change the next job's captured preference.
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 194 -Synchronous
            } else {
                Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4 -Synchronous
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 220 -DesignY 154 -Synchronous
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 390 -DesignY 154 -Synchronous
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
            }
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        }
        if ($scenario -ne 'Failure') { Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue }
        $length = (Get-Item -LiteralPath $logPath).Length
        $lineCount = @(Get-Content -LiteralPath $logPath).Count
        $outputLock = $null
        try {
            if ($scenario -eq 'Failure') {
                $outputLock = [System.IO.File]::Open($output, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
            }
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
            $eventText = if ($scenario -eq 'Disabled') { 'Compress: completed' } else { 'Operation summary shown: Compress' }
            Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message $eventText
            $newRows = @(Get-Content -LiteralPath $logPath -Encoding UTF8 | Select-Object -Skip $lineCount)
            $expectedResult = if ($scenario -eq 'Failure') { 'Compress: failed' } else { 'Compress: completed' }
            if (-not ($newRows | Where-Object { $_.Contains($expectedResult) })) {
                throw "Summary scenario $scenario did not report its expected worker result: $expectedResult."
            }
            $openRows = @($newRows | Where-Object { $_.Contains('Destination folder open suppressed (GUI smoke):') })
            $expectedOpens = if ($scenario -eq 'Enabled') { 1 } else { 0 }
            # Persistent log fields use reversible C-style escaping so control bytes cannot forge rows.
            $loggedDestination = $Destination.Replace('\', '\\')
            if ($openRows.Count -ne $expectedOpens -or
                ($expectedOpens -eq 1 -and -not $openRows[0].EndsWith($loggedDestination))) {
                throw "Summary scenario $scenario did not honor the captured destination-opening preference."
            }
        } finally {
            if ($null -ne $outputLock) { $outputLock.Dispose() }
        }
        if (-not (Test-Path -LiteralPath $output) -or (Get-Item -LiteralPath $output).Length -le 0) {
            throw 'Summary smoke compression did not produce an archive.'
        }
        if ($scenario -eq 'Enabled') { $firstHash = (Get-FileHash -LiteralPath $output).Hash }
        if ($scenario -eq 'Failure' -and (Get-FileHash -LiteralPath $output).Hash -ne $firstHash) {
            throw 'Locked-output failure changed the completed archive.'
        }
        $path = "${BasePath}-Summary-$scenario$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $path
        $offset = Get-ClientCaptureOffset -Handle $Handle
        $top = if ($scenario -eq 'Disabled') { 130 } else { 319 }
        Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 1 -Top $top -Right 4 -Bottom ($top + 42) -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 8 -MinPixels 100 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y
    }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 194 -Synchronous
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied'
    Assert-SettingsValue -Path $SettingsPath -Name 'showOperationSummary' -Expected $false
    Assert-SettingsValue -Path $SettingsPath -Name 'openDestinationAfterOperation' -Expected $false
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1130 -DesignY 91 -Synchronous
}

# Purpose: Verify failed Apply and Restore Defaults keep the last successfully saved settings active.
# Inputs: Handle/Dpi identify the window; Path is redirected settings; BasePath/Extension and Captures retain frames.
# Outputs: Exercises both save failures and throws if navigation promotes unsaved settings.
function Assert-SettingsSaveFailureRollback {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [string]$Path,
        [string]$BasePath,
        [string]$Extension,
        [ref]$Captures
    )

    $original = Get-Content -Raw -LiteralPath $Path
    $logPath = Join-Path (Split-Path -Parent $Path) "flitzzip.log"
    foreach ($command in @(
        @{ Name = "Apply"; X = 1110; Failure = "Settings apply failed" },
        @{ Name = "Restore Defaults"; X = 985; Failure = "Settings reset failed" }
    )) {
        # Select a different draft value before trying to save or reset it.
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 700 -DesignY 384
        Start-Sleep -Milliseconds 150
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 700 -DesignY 456
        Start-Sleep -Milliseconds 150
        $lockedFile = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try {
            $logLength = (Get-Item -LiteralPath $logPath).Length
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $command.X -DesignY 666
            Wait-GuiLogEvent -Path $logPath -PreviousLength $logLength -Message $command.Failure
            if ((Get-Content -Raw -LiteralPath $Path) -cne $original) {
                throw "Failed $($command.Name) changed the persisted settings."
            }
            $failureName = $command.Name.Replace(' ', '-')
            $capture = Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Settings-$failureName-Failure$Extension"
            $Captures.Value += $capture
            Assert-OperationStatusVisible -Handle $Handle -Dpi $Dpi -Path $capture.Path
        } finally {
            $lockedFile.Dispose()
        }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0
        Start-Sleep -Milliseconds 250
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6
        Start-Sleep -Milliseconds 250
        $logLength = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666
        Wait-GuiLogEvent -Path $logPath -PreviousLength $logLength -Message "Settings applied"
        if ((Get-Content -Raw -LiteralPath $Path) -cne $original) {
            throw "Failed $($command.Name) replaced the applied snapshot with unsaved settings."
        }
        Write-Output "Settings save-failure rollback passed: $($command.Name)."
    }
}

# Purpose: Require visible operation feedback inside its dedicated status-strip column.
# Inputs: Handle/Dpi identify the actual window; Path is its current native-rendered capture.
# Outputs: Rejects a blank column without counting borders, the GPU indicator, progress fields or clock pixels.
function Assert-OperationStatusVisible {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Path)

    [FlitzZipNativeUi+RECT]$client = New-Object FlitzZipNativeUi+RECT
    if (-not [FlitzZipNativeUi]::GetClientRect($Handle, [ref]$client)) {
        throw 'Cannot inspect the operation-status client geometry.'
    }
    $bottom = [int][Math]::Round($client.Bottom * 96.0 / $Dpi)
    $offset = Get-ClientCaptureOffset -Handle $Handle
    Assert-DesignRectHasDetail -Path $Path -Dpi $Dpi -Left 238 -Top ($bottom - 30) -Right 432 -Bottom ($bottom - 4) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 2
}

# Purpose: Exercise History overflow through real rows and each pointer scrolling path.
# Inputs: Handle/Dpi identify the smoke window; SettingsPath is isolated smoke storage; BasePath/Extension name captures.
# Outputs: Returns screenshots and throws if wheel, track clicks, or dragging cannot reach both table boundaries.
function Assert-HistoryScrollEndpoint {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1100 -DesignY 90
    foreach ($x in @(220, 390)) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $x -DesignY 145
        Start-Sleep -Milliseconds 100
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $x -DesignY 182
        Start-Sleep -Milliseconds 100
    }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6
    $logPath = Join-Path (Split-Path -Parent $SettingsPath) "flitzzip.log"
    foreach ($index in 1..20) {
        $logLength = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1110 -DesignY 666
        Wait-GuiLogEvent -Path $logPath -PreviousLength $logLength -Message "Settings applied and saved"
    }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4
    Start-Sleep -Milliseconds 200
    $offset = Get-ClientCaptureOffset -Handle $Handle
    foreach ($method in @('Wheel', 'Track', 'Drag')) {
        foreach ($direction in @('Bottom', 'Top')) {
            if ($method -eq 'Wheel') {
                $delta = if ($direction -eq 'Bottom') { -12000 } else { 12000 }
                Invoke-ClientWheel -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 400 -Delta $delta
            } elseif ($method -eq 'Track') {
                $y = if ($direction -eq 'Bottom') { 572 } else { 236 }
                foreach ($click in 1..4) {
                    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1158 -DesignY $y
                    Start-Sleep -Milliseconds 60
                }
            } else {
                $startY = if ($direction -eq 'Bottom') { 236 } else { 572 }
                $endY = if ($direction -eq 'Bottom') { 580 } else { 228 }
                Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX 1158 -StartY $startY -EndX 1158 -EndY $endY
            }
            # Move hover away so endpoint evidence uses the normal thumb color.
            [void][FlitzZipNativeUi]::PostMessage($Handle, 0x0200, [IntPtr]::Zero,
                (ConvertTo-MouseLParam -X 0 -Y 0))
            Start-Sleep -Milliseconds 250
            $path = "${BasePath}-History-$method-$direction$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $path
            $top = if ($direction -eq 'Bottom') { 570 } else { 234 }
            Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 1156 -Top $top -Right 1160 -Bottom ($top + 4) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 64 -ExpectedGreen 83 -ExpectedBlue 90 -Tolerance 12 -MinPixels 8
        }
    }
    Write-Information "History overflow endpoints passed: wheel, track click, and thumb drag." -InformationAction Continue
}

# Purpose: Resize only the owned smoke window using its actual native frame dimensions.
# Inputs: Handle/Dpi identify the test window; Width/Height are required client DIPs.
# Outputs: Returns explicitly named prior window/client pixel dimensions; throws unless the requested client size is reached.
function Invoke-SmokeClientResize {
    param([IntPtr]$Handle, [int]$Dpi, [int]$Width, [int]$Height)

    $window = New-Object FlitzZipNativeUi+RECT
    $client = New-Object FlitzZipNativeUi+RECT
    if (-not [FlitzZipNativeUi]::GetWindowRect($Handle, [ref]$window) -or
        -not [FlitzZipNativeUi]::GetClientRect($Handle, [ref]$client)) { throw 'Cannot inspect smoke viewport.' }
    $old = @{ WindowWidthPixels = $window.Right - $window.Left; WindowHeightPixels = $window.Bottom - $window.Top
        ClientWidthPixels = $client.Right; ClientHeightPixels = $client.Bottom }
    $physicalWidth = [int][Math]::Round($Width * $Dpi / 96.0)
    $physicalHeight = [int][Math]::Round($Height * $Dpi / 96.0)
    if (-not [FlitzZipNativeUi]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0,
        ($physicalWidth + $old.WindowWidthPixels - $client.Right), ($physicalHeight + $old.WindowHeightPixels - $client.Bottom), 0x0016)) {
        throw 'Cannot resize the owned smoke window.'
    }
    if (-not [FlitzZipNativeUi]::GetClientRect($Handle, [ref]$client) -or
        $client.Right -ne $physicalWidth -or $client.Bottom -ne $physicalHeight) { throw 'Client dimensions mismatch.' }
    return $old
}

# Purpose: Restore the owned window without applying DPI scaling to an already physical size.
# Inputs: Handle is the test HWND; Snapshot is the pixel-dimension record from Invoke-SmokeClientResize.
# Outputs: Restores the native outer size and requires exact original client pixels; failed Win32 operations throw.
function Restore-SmokeWindowSize {
    param([IntPtr]$Handle, [hashtable]$Snapshot)

    if ($Snapshot.WindowWidthPixels -le 0 -or $Snapshot.WindowHeightPixels -le 0 -or
        $Snapshot.ClientWidthPixels -le 0 -or $Snapshot.ClientHeightPixels -le 0) {
        throw 'Cannot restore an invalid smoke window pixel snapshot.'
    }
    if (-not [FlitzZipNativeUi]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0,
        $Snapshot.WindowWidthPixels, $Snapshot.WindowHeightPixels, 0x0016)) {
        throw 'Cannot restore the owned smoke window pixel dimensions.'
    }
    $client = New-Object FlitzZipNativeUi+RECT
    if (-not [FlitzZipNativeUi]::GetClientRect($Handle, [ref]$client) -or
        $client.Right -ne $Snapshot.ClientWidthPixels -or $client.Bottom -ne $Snapshot.ClientHeightPixels) {
        throw 'Restored smoke client pixels do not match the original snapshot.'
    }
}

# Purpose: Check the application's actual work-area fit, including notification-driven restoration.
# Inputs: Handle/Dpi identify the owned window; ExerciseEvents also tests work-area, display, and move notifications.
# Outputs: Fails wrong native size/position without changing host display settings or any other window.
function Assert-WindowWorkAreaFit {
    param([IntPtr]$Handle, [int]$Dpi, [switch]$ExerciseEvents)

    $events = if ($ExerciseEvents) { @(@(0x001A, 0x002F), @(0x007E, 0), @(0x0232, 0)) } else { @(@(0, 0)) }
    foreach ($notification in $events) {
        if ($ExerciseEvents) {
            [void](Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600)
            $result = [UIntPtr]::Zero
            if ([FlitzZipNativeUi]::SendMessageTimeout($Handle, $notification[0], [IntPtr]$notification[1], [IntPtr]::Zero,
                0x0002, 5000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Window fitting notification timed out.' }
        }
        $window = New-Object FlitzZipNativeUi+RECT
        $client = New-Object FlitzZipNativeUi+RECT
        $info = New-Object FlitzZipNativeUi+MONITORINFO
        $info.Size = [Runtime.InteropServices.Marshal]::SizeOf($info)
        if (-not [FlitzZipNativeUi]::GetWindowRect($Handle, [ref]$window) -or
            -not [FlitzZipNativeUi]::GetClientRect($Handle, [ref]$client) -or
            -not [FlitzZipNativeUi]::GetMonitorInfo([FlitzZipNativeUi]::MonitorFromWindow($Handle, 2), [ref]$info)) {
            throw 'Cannot inspect the actual monitor work area.'
        }
        $frameWidth = $window.Right - $window.Left - $client.Right
        $frameHeight = $window.Bottom - $window.Top - $client.Bottom
        $availableWidth = $info.Work.Right - $info.Work.Left - $frameWidth
        $availableHeight = $info.Work.Bottom - $info.Work.Top - $frameHeight
        $expectedWidth = [Math]::Max([Math]::Round(960 * $Dpi / 96.0), [Math]::Min([Math]::Round(1200 * $Dpi / 96.0), $availableWidth))
        $expectedHeight = [Math]::Max([Math]::Round(600 * $Dpi / 96.0), [Math]::Min([Math]::Round(760 * $Dpi / 96.0), $availableHeight))
        if ($client.Right -ne $expectedWidth -or $client.Bottom -ne $expectedHeight) { throw 'Window did not fit its work area.' }
        if ($availableWidth -ge $expectedWidth -and $availableHeight -ge $expectedHeight -and
            ($window.Left -lt $info.Work.Left -or $window.Top -lt $info.Work.Top -or
             $window.Right -gt $info.Work.Right -or $window.Bottom -gt $info.Work.Bottom)) {
            throw 'Window extends outside the available work area.'
        }
    }
    Write-Information "Work-area fitting passed; notification coverage=$ExerciseEvents." -InformationAction Continue
}

# Purpose: Fingerprint only a static text body, excluding clocks, tabs, borders and scrollbar hover.
# Inputs: Path is a window capture; Dpi/Offset map the supplied client-DIP rectangle into that image.
# Outputs: Returns the SHA-256 digest of its canonical pixel-format crop; invalid bounds fail.
function Get-GuiTextBodyDigest {
    param([string]$Path, [int]$Dpi, $Offset, [int]$Left, [int]$Top, [int]$Right, [int]$Bottom)

    $scale = [double]$Dpi / 96.0
    $rect = [Drawing.Rectangle]::FromLTRB(
        $Offset.X + [int][Math]::Round($Left * $scale), $Offset.Y + [int][Math]::Round($Top * $scale),
        $Offset.X + [int][Math]::Round($Right * $scale), $Offset.Y + [int][Math]::Round($Bottom * $scale))
    $image = [Drawing.Bitmap]::new($Path)
    $crop = $null
    $stream = [IO.MemoryStream]::new()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        if ($rect.Width -le 0 -or $rect.Height -le 0 -or $rect.Left -lt 0 -or $rect.Top -lt 0 -or
            $rect.Right -gt $image.Width -or $rect.Bottom -gt $image.Height) {
            throw 'GUI text-body comparison rectangle is outside the captured image.'
        }
        $crop = $image.Clone($rect, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $crop.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        return [BitConverter]::ToString($sha.ComputeHash($stream.ToArray()))
    } finally {
        $sha.Dispose()
        $stream.Dispose()
        if ($crop) { $crop.Dispose() }
        $image.Dispose()
    }
}

# Purpose: Wait for a visible text endpoint without repeating input or assuming animation completion from a delay.
# Inputs: Handle/Dpi identify the owned window; Body/Offset select static text; ExpectedDigest is independently captured.
# Outputs: Returns a matching capture or fails within a bounded observation loop, preserving the final mismatching image.
function Save-GuiTextEndpoint {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Path, $Offset, [hashtable]$Body,
          [string]$ExpectedDigest, [switch]$Compact)

    $captureSize = if ($Compact) { @{ ExpectedDesignWidth = 960; ExpectedDesignHeight = 600 } } else { @{} }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    for ($attempt = 0; $attempt -lt 6 -and $timer.Elapsed.TotalSeconds -lt 8; ++$attempt) {
        $capture = Save-FlitzZipScreenshot -Handle $Handle -Path $Path @captureSize
        if ((Get-GuiTextBodyDigest -Path $Path -Dpi $Dpi -Offset $Offset @Body) -ceq $ExpectedDigest) {
            return $capture
        }
        Start-Sleep -Milliseconds 80
    }
    throw "GUI text endpoint was not observed; final capture retained: $Path"
}

# Purpose: Require real thumb drags to reach the same notice text as independent Home/End inputs.
# Inputs: Handle/Dpi identify the owned modal; Viewport contains its canonical client-DIP boundaries.
# Outputs: Returns endpoint captures; throws for inert scrolling, wrong endpoints or failed restoration.
function Assert-LicenseNoticeDrag {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension,
          [hashtable]$Viewport, [switch]$Compact)

    $offset = Get-ClientCaptureOffset -Handle $Handle
    $body = @{ Left = $Viewport.Left + 2; Top = $Viewport.Top + 2
        Right = $Viewport.Right - 18; Bottom = $Viewport.Bottom - 2 }
    $captureSize = if ($Compact) { @{ ExpectedDesignWidth = 960; ExpectedDesignHeight = 600 } } else { @{} }
    $expected = @{}
    foreach ($endpoint in @('Top', 'Bottom', 'DragBottom', 'DragTop')) {
        if ($endpoint -eq 'Top') { Invoke-ClientKey -Handle $Handle -VirtualKey 0x24 }
        elseif ($endpoint -eq 'Bottom') { Invoke-ClientKey -Handle $Handle -VirtualKey 0x23 }
        else {
            if ($endpoint -eq 'DragBottom') {
                Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
                Save-GuiTextEndpoint -Handle $Handle -Dpi $Dpi -Path "$BasePath-HomeReady$Extension" -Offset $offset -Body $body -ExpectedDigest $expected.Top -Compact:$Compact
                $startY = $Viewport.Top + 8; $endY = $Viewport.Bottom + 32
            } else { $startY = $Viewport.Bottom - 8; $endY = $Viewport.Top - 32 }
            Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX ($Viewport.Right - 2) -EndX ($Viewport.Right - 2) -StartY $startY -EndY $endY
        }
        $path = "$BasePath-$endpoint$Extension"
        if ($endpoint -eq 'DragBottom' -or $endpoint -eq 'DragTop') {
            $target = if ($endpoint -eq 'DragBottom') { 'Bottom' } else { 'Top' }
            Save-GuiTextEndpoint -Handle $Handle -Dpi $Dpi -Path $path -Offset $offset -Body $body -ExpectedDigest $expected[$target] -Compact:$Compact
        } else {
            Start-Sleep -Milliseconds 220
            Save-FlitzZipScreenshot -Handle $Handle -Path $path @captureSize
        }
        $digest = Get-GuiTextBodyDigest -Path $path -Dpi $Dpi -Offset $offset @body
        if ($endpoint -eq 'Top' -or $endpoint -eq 'Bottom') { $expected[$endpoint] = $digest }
        else {
            $target = if ($endpoint -eq 'DragBottom') { 'Bottom' } else { 'Top' }
            if ($digest -cne $expected[$target]) { throw "License thumb $endpoint did not restore the keyboard endpoint text." }
        }
    }
    if ($expected.Top -ceq $expected.Bottom) { throw 'License notice Home and End displayed identical body pixels.' }
}

# Purpose: Exercise actual History detail overflow using a successfully extracted long Unicode archive name.
# Inputs: Handle/Dpi identify the owned window; fixture/output paths are isolated smoke storage; SourcePath supplies independent readback bytes.
# Outputs: Returns compact detail-scroll captures; rejects inert wheel/drag input and restores the original window dimensions.
function Assert-HistoryDetailsScroll {
    param([IntPtr]$Handle, [int]$Dpi, [string]$FixtureArchive, [string]$FixtureRoot,
          [string]$SourcePath, [string]$OutputRoot, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $name = 'History details ' + ([string][char]0x754C * 170) + '.flzip'
    $archive = Join-Path $FixtureRoot $name
    Copy-Item -LiteralPath $FixtureArchive -Destination $archive -Force
    Remove-GuiSmokeDirectory -Path $OutputRoot
    New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1100 -DesignY 90 -Synchronous
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
    Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($archive)
    Start-Sleep -Milliseconds 200
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
    $log = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $length = (Get-Item -LiteralPath $log).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
    Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Extract: completed'
    $restored = Join-Path $OutputRoot 'drag-drop-input.txt'
    if (-not (Test-Path -LiteralPath $restored) -or
        (Get-FileHash -LiteralPath $restored).Hash -ne (Get-FileHash -LiteralPath $SourcePath).Hash) {
        throw 'History scrolling fixture did not independently restore the real archive bytes.'
    }
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 4 -Synchronous
    foreach ($x in @(220, 390)) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX $x -DesignY 145 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        $count = if ($x -eq 220) { 2 } else { 1 }
        for ($index = 0; $index -lt $count; ++$index) { Invoke-ClientKey -Handle $Handle -VirtualKey 0x28 }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
    }
    Assert-HistoryKeyboardRow -Handle $Handle -Dpi $Dpi -BasePath $BasePath -Extension $Extension
    $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600
    try {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 420 -DesignY 239 -Synchronous
        $offset = Get-ClientCaptureOffset -Handle $Handle
        $body = @{ Left = 136; Top = 458; Right = 900; Bottom = 534 }
        $expected = @{}
        $positions = @(
            @{ Action = 'Top'; Image = 'History-Details-Top' },
            @{ Action = 'SmoothWheel'; Image = 'History-Details-SmoothWheel' },
            @{ Action = 'DragTop'; Image = 'History-Details-DragTop' },
            @{ Action = 'DragBottom'; Image = 'History-Details-DragBottom' })
        foreach ($case in $positions) {
            $position = $case.Action
            if ($position -eq 'SmoothWheel') {
                Invoke-ClientWheel -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 490 -Delta -12000
            } elseif ($position -eq 'DragTop') {
                Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX 920 -StartY 528 -EndX 920 -EndY 432
            } elseif ($position -eq 'DragBottom') {
                Invoke-ClientDrag -Handle $Handle -Dpi $Dpi -StartX 920 -StartY 464 -EndX 920 -EndY 560
            }
            Start-Sleep -Milliseconds 220
            $path = "${BasePath}-Compact-$($case.Image)$Extension"
            Save-FlitzZipScreenshot -Handle $Handle -Path $path -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
            $digest = Get-GuiTextBodyDigest -Path $path -Dpi $Dpi -Offset $offset @body
            if ($position -eq 'Top' -or $position -eq 'SmoothWheel') { $expected[$position] = $digest }
            else {
                $target = if ($position -eq 'DragTop') { 'Top' } else { 'SmoothWheel' }
                if ($digest -cne $expected[$target]) { throw "History details $position did not restore the independent wheel endpoint." }
            }
            if ($position -eq 'Top') {
                Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 918 -Top 458 -Right 923 -Bottom 468 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 64 -ExpectedGreen 83 -ExpectedBlue 90 -Tolerance 12 -MinPixels 2
            }
        }
        if ($expected.Top -ceq $expected.SmoothWheel) { throw 'History details wheel did not move the actual selected operation text.' }
    } finally {
        Restore-SmokeWindowSize -Handle $Handle -Snapshot $old
    }
    Write-Information 'History selected-operation overflow, independent archive readback, wheel and bidirectional thumb dragging passed.' -InformationAction Continue
}

# Purpose: Exercise the compact Licenses modal using native buttons, tabs, and scrolling.
# Inputs: Handle/Dpi identify a compact About page; BasePath/Extension name screenshots.
# Outputs: Returns validated modal captures; closes through the real Close button before later form checks.
function Assert-CompactLicenseDialog {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension, [string]$SettingsPath)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 434 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'License notices opened'
    $offset = Get-ClientCaptureOffset -Handle $Handle
    foreach ($tab in 0..1) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX (220 + 136 * $tab) -DesignY 176 -Synchronous
        $path = "${BasePath}-Compact-Licenses-$tab$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $path -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left (165 + 136 * $tab) -Top 159 -Right (293 + 136 * $tab) -Bottom 193 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
        Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left 175 -Top 209 -Right 867 -Bottom 443 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
        Assert-LicenseNoticeDrag -Handle $Handle -Dpi $Dpi -BasePath "${BasePath}-Compact-Licenses-$tab-Drag" -Extension $Extension -Compact -Viewport @{ Left = 175; Top = 209; Right = 867; Bottom = 443 }
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    Start-Sleep -Milliseconds 200
    Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-Licenses-End$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 826 -DesignY 481 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'License notices closed'
    $closedPath = "${BasePath}-Compact-Licenses-Closed$Extension"
    Save-FlitzZipScreenshot -Handle $Handle -Path $closedPath -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
    Assert-DesignRectHasColor -Path $closedPath -Dpi $Dpi -Left 160 -Top 180 -Right 240 -Bottom 260 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
}

# Purpose: Check overwrite confirmation and cancellation at the minimum client viewport.
# Inputs: Handle/Dpi identify the window with an open overwrite prompt; BasePath/Extension name screenshots.
# Outputs: Captures the compact prompt, clicks Cancel, verifies the Extract title returns, and restores the original size.
function Assert-CompactOverwritePrompt {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension)

    $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600
    try {
        $path = "${BasePath}-Compact-OverwritePrompt$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $path -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        $offset = Get-ClientCaptureOffset -Handle $Handle
        Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left 230 -Top 186 -Right 810 -Bottom 425 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 756 -DesignY 398 -Synchronous
        $closedPath = "${BasePath}-Compact-OverwriteCancelled$Extension"
        Save-FlitzZipScreenshot -Handle $Handle -Path $closedPath -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        Assert-DesignRectHasDetail -Path $closedPath -Dpi $Dpi -Left 116 -Top 74 -Right 350 -Bottom 110 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    } finally {
        Restore-SmokeWindowSize -Handle $Handle -Snapshot $old
    }
}

# Purpose: Verify the rendered overwrite modal focus or the restored Extract heading after dismissal.
# Inputs: Owned window/DPI, expected focus (Cancel/Allow/Closed), and unique screenshot path.
# Outputs: Returns the capture; throws if the expected native visual state is absent.
function Assert-OverwriteKeyboardFrame {
    param([IntPtr]$Handle, [int]$Dpi, [ValidateSet('Cancel', 'Allow', 'Closed')][string]$Focus, [string]$Path)

    Save-FlitzZipScreenshot -Handle $Handle -Path $Path
    $offset = Get-ClientCaptureOffset -Handle $Handle
    if ($Focus -eq 'Closed') {
        Assert-DesignRectHasDetail -Path $Path -Dpi $Dpi -Left 116 -Top 74 -Right 350 -Bottom 110 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
        # A heading alone also accepts Queue; require the independently located Extract rail selection.
        Assert-DesignRectHasColor -Path $Path -Dpi $Dpi -Left 1 -Top 190 -Right 6 -Bottom 248 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
    } else {
        # Independent 1200x760 design oracle: the focus border is above its action button.
        $left = if ($Focus -eq 'Cancel') { 830 } else { 680 }
        Assert-DesignRectHasColor -Path $Path -Dpi $Dpi -Left $left -Top 456 -Right ($left + 70) -Bottom 460 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 99 -ExpectedGreen 130 -ExpectedBlue 140 -Tolerance 2 -MinPixels 10
    }
}

# Purpose: Prove overwrite cancellation preserves the destination and approval restores exact archive bytes.
# Inputs: Isolated destination, expected payload SHA-256, and unchanged unrelated sentinel SHA-256.
# Outputs: Throws on extra/missing entries, changed sentinel, or wrong payload bytes.
function Assert-OverwriteDestination {
    param([string]$Directory, [string]$PayloadHash, [string]$SentinelHash)

    $entries = @(Get-ChildItem -LiteralPath $Directory -Recurse -Force)
    $names = @($entries | Select-Object -ExpandProperty Name | Sort-Object)
    if ($entries.Count -ne 2 -or @($entries | Where-Object PSIsContainer).Count -ne 0 -or
        ($names -join '|') -cne 'drag-drop-input.txt|existing-output.txt' -or
        (Get-FileHash -LiteralPath (Join-Path $Directory 'drag-drop-input.txt')).Hash -cne $PayloadHash -or
        (Get-FileHash -LiteralPath (Join-Path $Directory 'existing-output.txt')).Hash -cne $SentinelHash) {
        throw 'Overwrite confirmation changed the destination inventory or failed independent payload readback.'
    }
}

# Purpose: Qualify native overwrite keyboard cancellation, modal isolation, navigation and per-job approval.
# Inputs: Extract page with Ask policy and one archive; isolated destination/source/settings and capture paths.
# Outputs: Returns captures; proves cancellation preserves bytes and approval affects only its job.
function Assert-KeyboardOverwriteConfirmation {
    param([IntPtr]$Handle, [int]$Dpi, [string]$Directory, [string]$SourcePath,
          [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $payload = Join-Path $Directory 'drag-drop-input.txt'
    Set-Content -LiteralPath $payload -Value 'Unapproved replacement sentinel' -NoNewline
    $beforeHash = (Get-FileHash -LiteralPath $payload).Hash
    $sourceHash = (Get-FileHash -LiteralPath $SourcePath).Hash
    $sentinelHash = (Get-FileHash -LiteralPath (Join-Path $Directory 'existing-output.txt')).Hash
    if ($beforeHash -ceq $sourceHash) { throw 'Overwrite fixture must differ from the archive payload.' }
    $frame = @{ Handle = $Handle; Dpi = $Dpi }
    foreach ($key in @(0x0D, 0x20, 0x1B)) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
        Assert-OverwriteKeyboardFrame @frame -Focus Cancel -Path "${BasePath}-Overwrite-Key-$key-Open$Extension"
        Invoke-ClientKey -Handle $Handle -VirtualKey $key
        Assert-OverwriteKeyboardFrame @frame -Focus Closed -Path "${BasePath}-Overwrite-Key-$key-Closed$Extension"
        Assert-OverwriteDestination -Directory $Directory -PayloadHash $beforeHash -SentinelHash $sentinelHash
    }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x41
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Assert-OverwriteKeyboardFrame @frame -Focus Cancel -Path "${BasePath}-Overwrite-Isolated$Extension"
    $focus = 'Cancel'
    foreach ($key in @(0x09, 0x25, 0x27, 0x26, 0x28)) {
        Invoke-ClientKey -Handle $Handle -VirtualKey $key
        $focus = if ($focus -eq 'Cancel') { 'Allow' } else { 'Cancel' }
        Assert-OverwriteKeyboardFrame @frame -Focus $focus -Path "${BasePath}-Overwrite-Navigation-$key$Extension"
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x1B
    Assert-OverwriteKeyboardFrame @frame -Focus Closed -Path "${BasePath}-Overwrite-Navigation-Cancelled$Extension"
    Assert-OverwriteDestination -Directory $Directory -PayloadHash $beforeHash -SentinelHash $sentinelHash
    $log = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    foreach ($key in @(0x0D, 0x20)) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
        Assert-OverwriteKeyboardFrame @frame -Focus Cancel -Path "${BasePath}-Overwrite-Approval-$key-Open$Extension"
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x09
        Assert-OverwriteKeyboardFrame @frame -Focus Allow -Path "${BasePath}-Overwrite-Approval-$key-Focused$Extension"
        $length = (Get-Item -LiteralPath $log).Length
        Invoke-ClientKey -Handle $Handle -VirtualKey $key
        Wait-GuiLogEvent -Path $log -PreviousLength $length -Message 'Extract: completed'
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
        Assert-OverwriteKeyboardFrame @frame -Focus Closed -Path "${BasePath}-Overwrite-Approval-$key-Complete$Extension"
        Assert-OverwriteDestination -Directory $Directory -PayloadHash $sourceHash -SentinelHash $sentinelHash
        # A new job must ask again, default to Cancel, and retain the previous successful output.
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
        Assert-OverwriteKeyboardFrame @frame -Focus Cancel -Path "${BasePath}-Overwrite-Approval-$key-Reopened$Extension"
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        Assert-OverwriteKeyboardFrame @frame -Focus Closed -Path "${BasePath}-Overwrite-Approval-$key-Rejected$Extension"
        Assert-OverwriteDestination -Directory $Directory -PayloadHash $sourceHash -SentinelHash $sentinelHash
    }
}

# Purpose: Exercise the native legacy-name selector and verify real extraction paths and payloads.
# Inputs: The smoke window, native-test-exported CPIO fixture, shared output root, and capture naming.
# Outputs: Returns normal/compact captures; throws on missing menu, lost selection, or wrong extracted bytes.
function Assert-LegacyNameExtraction {
    param([IntPtr]$Handle, [int]$Dpi, [string]$FixtureRoot, [string]$OutputRoot, [string]$BasePath, [string]$Extension)

    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 0 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1134 -DesignY 91 -Synchronous
    $archives = @(Get-ChildItem -LiteralPath (Join-Path $FixtureRoot 'archive') -File)
    if ($archives.Count -ne 1) { throw 'Expected one legacy CPIO fixture.' }
    Invoke-FileDrop -Handle $Handle -Dpi $Dpi -Paths @($archives[0].FullName)
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 2 -Synchronous
    Invoke-DropdownExercise -Handle $Handle -Dpi $Dpi -Name 'Extract-NameEncoding-System' -OpenX 300 -OpenY 451 -SelectX 300 -SelectY 528 -MenuLeft 116 -MenuTop 478 -MenuRight 617 -MenuBottom 546 -BasePath $BasePath -Extension $Extension
    $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600
    try {
        Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-NameEncoding$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
    } finally {
        Restore-SmokeWindowSize -Handle $Handle -Snapshot $old
    }
    $logPath = Join-Path ([System.IO.Path]::GetTempPath()) 'FlitzZip/flitzzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 368 -Synchronous
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 1090 -DesignY 666 -Synchronous
    $loggedOutputRoot = $OutputRoot.Replace('\', '\\')
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message "Destination folder open suppressed (GUI smoke): $loggedOutputRoot"
    $expected = @(Get-ChildItem -LiteralPath (Join-Path $FixtureRoot 'expected') -File)
    foreach ($file in $expected) {
        $target = Join-Path $OutputRoot $file.Name
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Test-Path -LiteralPath $target) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $target) -or
            (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
            throw "GUI legacy-name extraction failed for $($file.Name)."
        }
    }
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 368 -Synchronous
    Invoke-DropdownExercise -Handle $Handle -Dpi $Dpi -Name 'Extract-NameEncoding-UTF8' -OpenX 300 -OpenY 451 -SelectX 300 -SelectY 494 -MenuLeft 116 -MenuTop 478 -MenuRight 617 -MenuBottom 546 -BasePath $BasePath -Extension $Extension
}

# Purpose: Verify compact popup scrolling through real selection and persistence.
# Inputs: Handle/Dpi identify a 960x600-DIP smoke window; SettingsPath is isolated storage; BasePath names screenshots.
# Outputs: Returns menu captures and fails when wheel, scroll arrows, or keyboard cannot select the last format.
function Assert-CompactFormatMenu {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
    foreach ($method in @('Wheel', 'Arrow', 'Keyboard')) {
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 218 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 218 -Synchronous
        if ($method -eq 'Wheel') {
            Invoke-ClientWheel -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 300 -Delta -1200
        } elseif ($method -eq 'Arrow') {
            foreach ($click in 1..3) {
                Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 540 -Synchronous
            }
        } else {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
        }
        Start-Sleep -Milliseconds 200
        Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-Format-$method$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        if ($method -eq 'Keyboard') {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        } else {
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 515 -Synchronous
        }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Assert-SettingsValue -Path $SettingsPath -Name 'compressionFormat' -Expected '.Z'
    }
}

# Purpose: Exercise form reflow without changing host resolution or monitor DPI.
# Inputs: Handle/Dpi identify the owned smoke window; SettingsPath is isolated storage; BasePath names captures.
# Outputs: Returns compact screenshots, verifies persisted controls, and restores the original client size in finally.
function Assert-CompactFormLayout {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $baselineJson = Get-Content -Raw -LiteralPath $SettingsPath
    $baseline = $baselineJson | ConvertFrom-Json
    $old = Invoke-SmokeClientResize -Handle $Handle -Dpi $Dpi -Width 960 -Height 600
    try {
        $names = @('Queue', 'Compress', 'Extract', 'Security', 'History', 'System', 'Settings', 'About')
        foreach ($index in 0..7) {
            Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex $index -Synchronous
            Start-Sleep -Milliseconds 150
            Save-FlitzZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-$($names[$index])$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        }
        Assert-CompactLicenseDialog -Handle $Handle -Dpi $Dpi -BasePath $BasePath -Extension $Extension -SettingsPath $SettingsPath
        Assert-CompactFormatMenu -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -BasePath $BasePath -Extension $Extension
        $original = Get-Content -Raw -LiteralPath $SettingsPath | ConvertFrom-Json
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 440 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'flitzzip.log'
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Assert-SettingsValue -Path $SettingsPath -Name 'showOperationSummary' -Expected (-not $original.showOperationSummary)
        Assert-SettingsValue -Path $SettingsPath -Name 'logRetentionDays' -Expected 7
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 440 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($row = 0; $row -lt (Get-GuiSettingsChoiceIndex -Settings $baseline -Kind Retention); ++$row) {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
        }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 218 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($row = 0; $row -lt (Get-GuiSettingsChoiceIndex -Settings $baseline -Kind Format); ++$row) {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
        }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        if ((Get-Content -Raw -LiteralPath $SettingsPath) -cne $baselineJson) {
            throw 'Compact smoke did not restore the original applied settings.'
        }
        Write-Information 'Compact forms and popup selections passed at the actual host DPI.' -InformationAction Continue
    } finally {
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x1B
        Restore-SmokeWindowSize -Handle $Handle -Snapshot $old
    }
}

$smokeRoot = Join-Path $repo "out\gui-smoke-work"
$smokeDestination = Join-Path $smokeRoot ("FlitzZip-destination-" + [char]0x03A9)
New-Item -ItemType Directory -Force -Path $smokeRoot | Out-Null
$smokeInput = Join-Path $smokeRoot "drag-drop-input.txt"
$secondArchiveRoot = Join-Path $smokeRoot "second-archive"
New-Item -ItemType Directory -Force -Path $secondArchiveRoot | Out-Null
$smokeInputTwo = Join-Path $secondArchiveRoot "drag-drop-input.txt"
$smokeFolder = Join-Path $smokeRoot "folder-input"
$smokeArchive = Join-Path $smokeRoot "valid-input.flzip"
$smokeArchiveTwo = Join-Path $secondArchiveRoot "VALID-INPUT.flzip"
$badArchive = Join-Path $smokeRoot "invalid-input.flzip"
$overflowFiles = @()
$bulkDropFiles = @()
$smokeCloseFile = Join-Path $smokeRoot "close.request"
$smokeSettingsDir = Join-Path ([System.IO.Path]::GetTempPath()) "FlitzZip"
$smokeSettingsFile = Join-Path $smokeSettingsDir "gui-smoke-settings.json"
New-Item -ItemType Directory -Force -Path $smokeSettingsDir | Out-Null
Remove-GuiSmokeDirectory -Path $smokeDestination
New-Item -ItemType Directory -Force -Path $smokeDestination | Out-Null
# A bounded correctness fixture exercises dictionary/entropy device readback; it is not a performance benchmark.
Write-NeutronGuiFixture -Path $smokeInput
Set-Content -LiteralPath $smokeInputTwo -Value "Second FlitzZip GUI smoke input" -NoNewline
New-Item -ItemType Directory -Force -Path $smokeFolder | Out-Null
Set-Content -LiteralPath (Join-Path $smokeFolder "nested.txt") -Value "Nested GUI smoke input" -NoNewline
foreach ($index in 1..28) {
    $path = Join-Path $smokeRoot ("overflow-{0:D2}.txt" -f $index)
    Set-Content -LiteralPath $path -Value "Queue overflow smoke item $index" -NoNewline
    $overflowFiles += (Resolve-Path -LiteralPath $path).Path
}
foreach ($index in 1..72) {
    $path = Join-Path $smokeRoot ("bulk-drop-{0:D2}.txt" -f $index)
    Set-Content -LiteralPath $path -Value "Bulk Queue drag/drop smoke item $index" -NoNewline
    $bulkDropFiles += (Resolve-Path -LiteralPath $path).Path
}
$queuePickerSelection = (@((Resolve-Path -LiteralPath $smokeInput).Path) + $overflowFiles) -join ';'
Set-Content -LiteralPath $badArchive -Value "not a valid FlitzZip archive" -NoNewline
Remove-Item -LiteralPath $smokeArchive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeArchiveTwo -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeSettingsFile -Force -ErrorAction SilentlyContinue
& (Join-Path $repo "build\$Configuration\flitzzip_cli.exe") compress --format flzip --output $smokeArchive --force-cpu $smokeInput | Out-Null
& (Join-Path $repo "build\$Configuration\flitzzip_cli.exe") compress --format flzip --output $smokeArchiveTwo --force-cpu $smokeInputTwo | Out-Null
if (-not (Test-Path -LiteralPath $smokeArchive)) {
    throw "Could not create valid FLZIP archive for GUI extract smoke."
}
if (-not (Test-Path -LiteralPath $smokeArchiveTwo)) {
    throw "Could not create second valid FLZIP archive for GUI multi-extract smoke."
}
$legacyFixture = Join-Path $smokeRoot ('legacy-names-' + [guid]::NewGuid().ToString('N'))
$previousFixtureExport = $env:FLITZZIP_TEST_FIXTURE_EXPORT
try {
    $env:FLITZZIP_TEST_FIXTURE_EXPORT = $legacyFixture
    $fixtureOutput = & (Join-Path $repo "build\$Configuration\flitzzip_tests.exe") cpio_legacy_member_names_both_passes
    if ($LASTEXITCODE -ne 0) { throw "Could not export the verified legacy-name GUI fixture: $($fixtureOutput -join '`n')" }
} finally {
    $env:FLITZZIP_TEST_FIXTURE_EXPORT = $previousFixtureExport
}
$previousSmokeDestination = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_DESTINATION", "Process")
$previousSmokeFiles = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_FILE_SELECTION", "Process")
$previousSmokeFolder = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_FOLDER_SELECTION", "Process")
$previousSmokeAutoClose = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS", "Process")
$previousSmokeCloseFile = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_CLOSE_FILE", "Process")
$previousSmokeSettingsRedirect = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT", "Process")
$previousSmokeShellOpen = [Environment]::GetEnvironmentVariable("FLITZZIP_GUI_SMOKE_SUPPRESS_SHELL_OPEN", "Process")
$smokeCleanupTimeoutMs = 10000
$smokeAutoCloseMs = Get-GuiSmokeWatchdogInterval -ScenarioTimeoutSeconds $ScenarioTimeoutSeconds -CleanupTimeoutMilliseconds $smokeCleanupTimeoutMs
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_DESTINATION", $smokeDestination, "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_FILE_SELECTION", $queuePickerSelection, "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_FOLDER_SELECTION", (Resolve-Path -LiteralPath $smokeFolder).Path, "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS", [string]$smokeAutoCloseMs, "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_CLOSE_FILE", $smokeCloseFile, "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT", "1", "Process")
[Environment]::SetEnvironmentVariable("FLITZZIP_GUI_SMOKE_SUPPRESS_SHELL_OPEN", "1", "Process")

$previousDpiContext = [FlitzZipNativeUi]::SetThreadDpiAwarenessContext([IntPtr](-4))
$process = $null
$windowHandle = [IntPtr]::Zero
try {
    $nativePickerCaptures = @()
    if (-not $CompactOnly -and -not $OperationSummaryOnly -and -not $HoverOnly -and -not $StatusFeedbackOnly) {
        Assert-PublicationSettingsMigration -Exe $exe -SettingsPath $smokeSettingsFile
        $nativeBasePath = Join-Path (Split-Path -Parent $ScreenshotPath) ([IO.Path]::GetFileNameWithoutExtension($ScreenshotPath))
        $nativeExtension = [IO.Path]::GetExtension($ScreenshotPath)
        $nativePickerCaptures = @(Assert-NativePickerQueue -Exe $exe -Root $smokeRoot -BasePath $nativeBasePath -Extension $nativeExtension -VerifyQueue {
            param($Handle, $Dpi, $Destination, $Expected)
            Assert-GuiCreatedZip -Handle $Handle -Dpi $Dpi -Destination $Destination -Expected $Expected -BasePath $nativeBasePath -Extension $nativeExtension
        })
        # The picker scenario changes its own compression choice; the all-page run starts with default smoke preferences.
        Remove-Item -LiteralPath $smokeSettingsFile -Force -ErrorAction SilentlyContinue
    }
    $process = Start-Process -FilePath $exe -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            throw "FlitzZip exited before showing a window. ExitCode=$($process.ExitCode)."
        }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($null -ne $process.MainWindowHandle -and $process.MainWindowHandle -ne 0) {
            $windowHandle = [IntPtr]$process.MainWindowHandle
            break
        }
    }
    if ($windowHandle -eq [IntPtr]::Zero) {
        throw "FlitzZip window did not appear."
    }

    Start-Sleep -Seconds 2
    $windowDpi = [int][FlitzZipNativeUi]::GetDpiForWindow($windowHandle)
    if ($windowDpi -le 0) {
        $windowDpi = 96
    }
    Assert-FixedWindowStyle -Handle $windowHandle
    Assert-WindowWorkAreaFit -Handle $windowHandle -Dpi $windowDpi
    Assert-WindowWorkAreaFit -Handle $windowHandle -Dpi $windowDpi -ExerciseEvents
    [void](Invoke-SmokeClientResize -Handle $windowHandle -Dpi $windowDpi -Width 1200 -Height 760)
    # The smoke observes Debug events. Configure isolated test settings explicitly, never product defaults.
    Set-GuiSmokeLogLevel -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -Index 2
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0 -Synchronous
    $captures = @($nativePickerCaptures)
    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "System", "Settings", "About")
    $basePath = Join-Path (Split-Path -Parent $ScreenshotPath) ([System.IO.Path]::GetFileNameWithoutExtension($ScreenshotPath))
    $extension = [System.IO.Path]::GetExtension($ScreenshotPath)
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $ScreenshotPath

    if ($StatusFeedbackOnly) {
        Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 6 -Synchronous
        Assert-SettingsSaveFailureRollback -Handle $windowHandle -Dpi $windowDpi -Path $smokeSettingsFile -BasePath $basePath -Extension $extension -Captures ([ref]$captures)
        $captures | ConvertTo-Json
        return
    }

    if ($CompactOnly) {
        $captures += Assert-CompactFormLayout -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
        $captures | Format-Table -AutoSize
        return
    }

    if ($HoverOnly) {
        $captures += Assert-GuiTextTooltip -Handle $windowHandle -Dpi $windowDpi -FixtureArchive $smokeArchive -FixtureRoot $smokeRoot -BasePath $basePath -Extension $extension -SkipHistory
        $captures | Format-Table -AutoSize
        return
    }

    $captures += Assert-OperationSummary -Handle $windowHandle -Dpi $windowDpi -InputPath $smokeInput -Destination $smokeDestination -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    if ($OperationSummaryOnly) { $captures | Format-Table -AutoSize; return }

    $captures += Assert-KeyboardQueueSelection -Handle $windowHandle -Dpi $windowDpi -InputPath $smokeInput -BasePath $basePath -Extension $extension
    $captures += Assert-QueueDropAdmission -Handle $windowHandle -Dpi $windowDpi -InputPath $smokeInput -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    $captures += Assert-MixedQueueRemoval -Handle $windowHandle -Dpi $windowDpi -Paths (@($smokeInput) + $overflowFiles[0..2]) -Destination $smokeDestination -BasePath $basePath -Extension $extension

    # Keep the directory visible: appending it below the overflow files never renders its
    # asynchronous total and leaves the folder-size worker outside this integration check.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1032 -DesignY 91
    Start-Sleep -Milliseconds 500
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 918 -DesignY 91
    Start-Sleep -Milliseconds 150
    $pickerQueuePath = "${basePath}-Queue-AfterPickers$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $pickerQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $pickerQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Assert-DesignRectHasDetail -Path $pickerQueuePath -Dpi $windowDpi -Left 1148 -Top 170 -Right 1166 -Bottom 640 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 3
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 505 -DesignY 91
    Start-Sleep -Milliseconds 180
    $removeSelectedQueuePath = "${basePath}-Queue-AfterRemoveSelected$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $removeSelectedQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $removeSelectedQueuePath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y
    # Re-add the directory after removal to exercise its new queue/cache lifetime.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1032 -DesignY 91
    Start-Sleep -Milliseconds 500
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 918 -DesignY 91
    Start-Sleep -Milliseconds 150
    $pickerReloadedQueuePath = "${basePath}-Queue-AfterPickersReloaded$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $pickerReloadedQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $pickerReloadedQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-ClientDrag -Handle $windowHandle -Dpi $windowDpi -StartX 1158 -StartY 186 -EndX 1158 -EndY 344
    Start-Sleep -Milliseconds 180
    $scrolledQueuePath = "${basePath}-Queue-AfterScrollbarDrag$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $scrolledQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $scrolledQueuePath -Dpi $windowDpi -Left 126 -Top 132 -Right 620 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    $emptyQueuePath = "${basePath}-Queue-EmptyDropZone$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $emptyQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $emptyQueuePath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y

    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths $bulkDropFiles
    Start-Sleep -Milliseconds 450
    $bulkDropQueuePath = "${basePath}-Queue-AfterBulkDragDrop$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $bulkDropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $bulkDropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Assert-DesignRectHasDetail -Path $bulkDropQueuePath -Dpi $windowDpi -Left 1148 -Top 170 -Right 1166 -Bottom 640 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 3
    $captures += Assert-QueueKeyboardScrolling -Handle $windowHandle -Dpi $windowDpi -Paths $bulkDropFiles -Destination $smokeDestination -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    $emptyAfterBulkDropPath = "${basePath}-Queue-AfterBulkDropClear$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $emptyAfterBulkDropPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $emptyAfterBulkDropPath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y

    # Queue: exercise drag/drop and row selection only. Destination, level, and Start belong to Compress/Extract.
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $smokeInput).Path)
    Start-Sleep -Milliseconds 350
    $dropQueuePath = "${basePath}-Queue-AfterDragDrop$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $dropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $dropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 136 -DesignY 146
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 136 -DesignY 146
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 136 -DesignY 184
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 136 -DesignY 184
    Start-Sleep -Milliseconds 120
    Invoke-ClientDrag -Handle $windowHandle -Dpi $windowDpi -StartX 420 -StartY 146 -EndX 455 -EndY 146
    Start-Sleep -Milliseconds 180
    $queueColumnPath = "${basePath}-Queue-AfterTicksAndResize$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $queueColumnPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $queueColumnPath -Dpi $windowDpi -Left 126 -Top 132 -Right 620 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 240 -DesignY 172
    Start-Sleep -Milliseconds 120

    # Compress: exercise fields, dropdowns, checkboxes, toggles, and Start.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 1
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Format" -OpenX 500 -OpenY 224 -SelectX 500 -SelectY 268 -MenuLeft 116 -MenuTop 252 -MenuRight 617 -MenuBottom 622 -BasePath $basePath -Extension $extension
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 0
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Level" -OpenX 820 -OpenY 224 -SelectX 820 -SelectY 390 -MenuLeft 657 -MenuTop 252 -MenuRight 1158 -MenuBottom 542 -BasePath $basePath -Extension $extension
    Assert-CompressionEffortSelection -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile
    Assert-NeutronSelectionIsolation -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension -InputPath $smokeInput -Destination $smokeDestination -Captures ([ref]$captures)
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Method" -OpenX 500 -OpenY 294 -SelectX 500 -SelectY 370 -MenuLeft 116 -MenuTop 322 -MenuRight 617 -MenuBottom 388 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-BlockSize" -OpenX 820 -OpenY 294 -SelectX 820 -SelectY 498 -MenuLeft 657 -MenuTop 322 -MenuRight 1158 -MenuBottom 548 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 432
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 512
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 512
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 547
    Start-Sleep -Milliseconds 140
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 547
    Start-Sleep -Milliseconds 140
    # Zstandard is outside the formats that support read-back verification.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 432
    Start-Sleep -Milliseconds 140
    $expectedZstd = Join-Path $smokeDestination "FlitzZip-output.zst"
    Remove-Item -LiteralPath $expectedZstd -Force -ErrorAction SilentlyContinue
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 6
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $createdZstd = $false
    foreach ($attempt in 1..50) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $expectedZstd) -and ((Get-Item -LiteralPath $expectedZstd).Length -gt 0)) {
            $createdZstd = $true
            break
        }
    }
    if (-not $createdZstd) {
        throw "GUI compression did not create expected non-empty Zstandard archive at $expectedZstd."
    }
    Start-Sleep -Milliseconds 300

    $expectedTarZstd = Join-Path $smokeDestination "FlitzZip-output.tar.zst"
    Remove-Item -LiteralPath $expectedTarZstd -Force -ErrorAction SilentlyContinue
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 8
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $createdTarZstd = $false
    foreach ($attempt in 1..50) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $expectedTarZstd) -and ((Get-Item -LiteralPath $expectedTarZstd).Length -gt 0)) {
            $createdTarZstd = $true
            break
        }
    }
    if (-not $createdTarZstd) {
        throw "GUI compression did not create expected non-empty TAR.ZST archive at $expectedTarZstd."
    }
    Start-Sleep -Milliseconds 300

    $expectedTarGz = Join-Path $smokeDestination "FlitzZip-output.tar.gz"
    Remove-Item -LiteralPath $expectedTarGz -Force -ErrorAction SilentlyContinue
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 2
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

    $expectedCpioGz = Join-Path $smokeDestination "FlitzZip-output.cpgz"
    Remove-Item -LiteralPath $expectedCpioGz -Force -ErrorAction SilentlyContinue
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 16
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $createdCpioGz = $false
    foreach ($attempt in 1..50) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path -LiteralPath $expectedCpioGz) -and ((Get-Item -LiteralPath $expectedCpioGz).Length -gt 0)) {
            $createdCpioGz = $true
            break
        }
    }
    if (-not $createdCpioGz) {
        throw "GUI compression did not create expected non-empty CPIO.GZ archive at $expectedCpioGz."
    }

    # Extract: return to Queue, clear inputs, drop a valid archive, then exercise extract controls.
    $extractOutput = $smokeDestination
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $smokeArchive).Path)
    Start-Sleep -Milliseconds 250
    $archiveDropQueuePath = "${basePath}-Queue-ArchiveAfterDragDrop$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $archiveDropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $archiveDropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 560 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 520 -DesignY 227
    Start-Sleep -Milliseconds 120
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 300 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 337
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 368
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 368
    Start-Sleep -Milliseconds 80
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 338
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 338
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 374
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 374
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $singleExtracted = Join-Path $extractOutput "drag-drop-input.txt"
    foreach ($attempt in 1..50) {
        if (Test-Path -LiteralPath $singleExtracted) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $singleExtracted)) {
        throw "GUI single-archive extraction did not restore expected file at $singleExtracted."
    }
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2 -Synchronous
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 175 -DesignY 337 -Synchronous

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Remove-GuiSmokeDirectory -Path $extractOutput
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @(
        (Resolve-Path -LiteralPath $smokeArchive).Path,
        (Resolve-Path -LiteralPath $smokeArchiveTwo).Path
    )
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    $multiFirst = Join-Path $extractOutput "valid-input\drag-drop-input.txt"
    $multiSecond = Join-Path $extractOutput "VALID-INPUT (2)\drag-drop-input.txt"
    foreach ($attempt in 1..50) {
        if ((Test-Path -LiteralPath $multiFirst) -and (Test-Path -LiteralPath $multiSecond)) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $multiFirst) -or -not (Test-Path -LiteralPath $multiSecond)) {
        throw "GUI multi-archive extraction did not restore both expected files under $extractOutput."
    }
    if ((Get-FileHash -LiteralPath $multiFirst -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $smokeInput -Algorithm SHA256).Hash -or
        (Get-FileHash -LiteralPath $multiSecond -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $smokeInputTwo -Algorithm SHA256).Hash) {
        throw 'GUI batch extraction merged or changed same-name members from case-equivalent archives.'
    }

    $captures += Assert-LegacyNameExtraction -Handle $windowHandle -Dpi $windowDpi -FixtureRoot $legacyFixture -OutputRoot $extractOutput -BasePath $basePath -Extension $extension

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $smokeArchive).Path)
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Remove-GuiSmokeDirectory -Path $extractOutput
    New-Item -ItemType Directory -Force -Path $extractOutput | Out-Null
    Set-Content -LiteralPath (Join-Path $extractOutput "existing-output.txt") -Value "Existing extraction output" -NoNewline
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite-Ask" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 266 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Milliseconds 250
    $overwritePromptPath = "${basePath}-Extract-OverwritePrompt$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $overwritePromptPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $overwritePromptPath -Dpi $windowDpi -Left 290 -Top 250 -Right 910 -Bottom 510 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    $captures += Assert-CompactOverwritePrompt -Handle $windowHandle -Dpi $windowDpi -BasePath $basePath -Extension $extension
    if (@(Get-ChildItem -LiteralPath $extractOutput -File).Count -ne 1 -or
        (Get-Content -Raw -LiteralPath (Join-Path $extractOutput 'existing-output.txt')) -cne 'Existing extraction output') {
        throw 'Cancelling compact overwrite confirmation modified the destination.'
    }
    $captures += Assert-KeyboardOverwriteConfirmation -Handle $windowHandle -Dpi $windowDpi -Directory $extractOutput -SourcePath $smokeInput -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    Start-Sleep -Milliseconds 300
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite-Revert" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 300 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension

    # Extract failure: queue a corrupt FLZIP and verify History exposes a real Failure row.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Remove-GuiSmokeDirectory -Path (Join-Path $smokeRoot "FlitzZip-extracted")
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $badArchive).Path)
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 520 -DesignY 227
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Seconds 2

    # Security, History, System, and Settings page controls.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 3
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Milliseconds 250

    $captures += Assert-UnicodeSecurityReview -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -FixtureArchive $smokeArchive -FixtureRoot $smokeRoot -BasePath $basePath -Extension $extension

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 4
    Start-Sleep -Milliseconds 250
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Operation" -OpenX 220 -OpenY 145 -SelectX 220 -SelectY 246 -MenuLeft 116 -MenuTop 170 -MenuRight 336 -MenuBottom 300 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Status-Success" -OpenX 390 -OpenY 145 -SelectX 390 -SelectY 214 -MenuLeft 354 -MenuTop 170 -MenuRight 574 -MenuBottom 268 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Status-Failure" -OpenX 390 -OpenY 145 -SelectX 390 -SelectY 246 -MenuLeft 354 -MenuTop 170 -MenuRight 574 -MenuBottom 268 -BasePath $basePath -Extension $extension
    $historyFailurePath = "${basePath}-History-FailureRows$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $historyFailurePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasColor -Path $historyFailurePath -Dpi $windowDpi -Left 700 -Top 210 -Right 1138 -Bottom 470 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 236 -ExpectedGreen 73 -ExpectedBlue 73 -Tolerance 42 -MinPixels 4
    $captures += Assert-HistoryDetailsScroll -Handle $windowHandle -Dpi $windowDpi -FixtureArchive $smokeArchive -FixtureRoot $smokeRoot -SourcePath $smokeInput -OutputRoot $extractOutput -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    $captures += Assert-GuiHistoryColumnResize -Handle $windowHandle -Dpi $windowDpi -BasePath $basePath -Extension $extension
    $captures += Assert-GuiTextTooltip -Handle $windowHandle -Dpi $windowDpi -FixtureArchive $smokeArchive -FixtureRoot $smokeRoot -BasePath $basePath -Extension $extension
    $captures += Assert-GuiQueueColumnResize -Handle $windowHandle -Dpi $windowDpi -BasePath $basePath -Extension $extension
    $captures += Assert-GuiResizeCaptureCancellation -Handle $windowHandle -Dpi $windowDpi -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1100 -DesignY 90
    Start-Sleep -Milliseconds 250

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 5
    Start-Sleep -Milliseconds 250
    $systemLogPath = Join-Path (Split-Path -Parent $smokeSettingsFile) "flitzzip.log"
    foreach ($choice in @(@{ Seconds = 3; Y = 312 }, @{ Seconds = 5; Y = 344 },
            @{ Seconds = 10; Y = 376 }, @{ Seconds = 1; Y = 280 })) {
        $logLength = (Get-Item -LiteralPath $systemLogPath).Length
        $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "System-UpdateSpeed-$($choice.Seconds)" -OpenX 1110 -OpenY 245 -SelectX 1110 -SelectY $choice.Y -MenuLeft 1072 -MenuTop 264 -MenuRight 1150 -MenuBottom 394 -BasePath $basePath -Extension $extension
        Wait-GuiLogEvent -Path $systemLogPath -PreviousLength $logLength -Message "System refresh interval: $($choice.Seconds) s"
    }
    $fixedDrives = @([System.IO.DriveInfo]::GetDrives() |
        Where-Object { $_.DriveType -eq [System.IO.DriveType]::Fixed } | Sort-Object Name)
    if ($fixedDrives.Count -eq 0) { throw "System I/O smoke requires a fixed local drive." }
    $firstDrive = $fixedDrives[0].Name.TrimEnd('\')
    $logLength = (Get-Item -LiteralPath $systemLogPath).Length
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "System-IODrive" -OpenX 1106 -OpenY 297 -SelectX 1106 -SelectY 328 -MenuLeft 1074 -MenuTop 312 -MenuRight 1138 -MenuBottom 344 -BasePath $basePath -Extension $extension
    Wait-GuiLogEvent -Path $systemLogPath -PreviousLength $logLength -Message "System I/O drive: $firstDrive"
    $systemMonitorPath = "${basePath}-System-PerformanceMonitor$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $systemMonitorPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $systemMonitorPath -Dpi $windowDpi -Left 146 -Top 348 -Right 1138 -Bottom 678 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 308 -Right 1138 -Bottom 678 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 63 -ExpectedGreen 181 -ExpectedBlue 221
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 308 -Right 1138 -Bottom 678 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 83 -ExpectedGreen 210 -ExpectedBlue 101
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 308 -Right 1138 -Bottom 678 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 237 -ExpectedGreen 179 -ExpectedBlue 61
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 308 -Right 1138 -Bottom 678 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 6
    Start-Sleep -Milliseconds 250
    $logLength = (Get-Item -LiteralPath $systemLogPath).Length
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1110 -DesignY 666
    Wait-GuiLogEvent -Path $systemLogPath -PreviousLength $logLength -Message "Settings applied"
    Assert-SettingsValue -Path $smokeSettingsFile -Name "performanceUpdateSeconds" -Expected 1
    Write-Output "System dropdown actions and refresh persistence passed."
    foreach ($point in @(
        @(175, 193),
        @(175, 227),
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
    foreach ($rowY in @(424, 456, 488)) {
        $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogLevel-$rowY" -OpenX 700 -OpenY 384 -SelectX 700 -SelectY $rowY -MenuLeft 622 -MenuTop 412 -MenuRight 887 -MenuBottom 510 -BasePath $basePath -Extension $extension
    }
    foreach ($rowY in @(482, 514, 546)) {
        $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogRetention-$rowY" -OpenX 700 -OpenY 442 -SelectX 700 -SelectY $rowY -MenuLeft 622 -MenuTop 470 -MenuRight 887 -MenuBottom 568 -BasePath $basePath -Extension $extension
    }
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1110 -DesignY 666
    Start-Sleep -Milliseconds 250
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logLevel" -Expected 'debug'
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logRetentionDays" -Expected 30

    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogLevel-UnappliedWarning" -OpenX 700 -OpenY 384 -SelectX 700 -SelectY 456 -MenuLeft 622 -MenuTop 412 -MenuRight 887 -MenuBottom 510 -BasePath $basePath -Extension $extension
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 6
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1110 -DesignY 666
    Start-Sleep -Milliseconds 250
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logLevel" -Expected 'debug'

    Assert-SettingsSaveFailureRollback -Handle $windowHandle -Dpi $windowDpi -Path $smokeSettingsFile -BasePath $basePath -Extension $extension -Captures ([ref]$captures)

    $captures += Assert-HistoryScrollEndpoint -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    $captures += Assert-CompactFormLayout -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension

    for ($index = 0; $index -lt $pageNames.Count; ++$index) {
        Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex $index
        Start-Sleep -Milliseconds 300
        $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path "${basePath}-$($pageNames[$index])$extension"
    }
    $captures += Assert-LicenseKeyboardNavigation -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1070 -DesignY 594
    Start-Sleep -Milliseconds 180
    $licenseDialogPath = "${basePath}-About-Licenses$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $licenseDialogPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $licenseDialogPath -Dpi $windowDpi -Left 260 -Top 120 -Right 940 -Bottom 650 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Assert-DesignRectHasColor -Path $licenseDialogPath -Dpi $windowDpi -Left 286 -Top 170 -Right 400 -Bottom 212 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
    $captures += Assert-LicenseNoticeDrag -Handle $windowHandle -Dpi $windowDpi -BasePath "${basePath}-About-Licenses-FlitzZip-Drag" -Extension $extension -Viewport @{ Left = 277; Top = 231; Right = 1005; Bottom = 581 }
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 450 -DesignY 198
    Start-Sleep -Milliseconds 120
    $licenseOtherDialogPath = "${basePath}-About-Licenses-Other$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $licenseOtherDialogPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $licenseOtherDialogPath -Dpi $windowDpi -Left 260 -Top 120 -Right 940 -Bottom 650 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    $captures += Assert-LicenseNoticeDrag -Handle $windowHandle -Dpi $windowDpi -BasePath "${basePath}-About-Licenses-Other-Drag" -Extension $extension -Viewport @{ Left = 277; Top = 231; Right = 1005; Bottom = 581 }
    Invoke-ClientWheel -Handle $windowHandle -Dpi $windowDpi -DesignX 610 -DesignY 420 -Delta -120
    Start-Sleep -Milliseconds 80
    $licenseSmoothWheelPath = "${basePath}-About-Licenses-SmoothWheel$extension"
    $captures += Save-FlitzZipScreenshot -Handle $windowHandle -Path $licenseSmoothWheelPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $licenseSmoothWheelPath -Dpi $windowDpi -Left 260 -Top 120 -Right 940 -Bottom 650 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Invoke-ClientKey -Handle $windowHandle -VirtualKey 0x22
    Start-Sleep -Milliseconds 100
    Invoke-ClientKey -Handle $windowHandle -VirtualKey 0x1B
    Start-Sleep -Milliseconds 120
    $captures | ConvertTo-Json
} finally {
    $launchedProcessId = if ($process) { $process.Id } else { 0 }
    $cleanupFailure = $null
    Set-Content -LiteralPath $smokeCloseFile -Value "close" -NoNewline
    if ($process -and -not $process.HasExited) {
        if ($windowHandle -ne [IntPtr]::Zero) {
            [void][FlitzZipNativeUi]::PostMessage($windowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        if (-not $process.WaitForExit($smokeCleanupTimeoutMs)) {
            try {
                Stop-Process -Id $process.Id -Force
            } catch {
                Write-Warning "Could not force-stop FlitzZip process $($process.Id): $($_.Exception.Message)"
            }
        }
    }
    if ($launchedProcessId -ne 0 -and (Get-Process -Id $launchedProcessId -ErrorAction SilentlyContinue)) {
        $cleanupFailure = "FlitzZip GUI smoke process $launchedProcessId did not exit cleanly."
    }
    if ($previousDpiContext -ne [IntPtr]::Zero) {
        [void][FlitzZipNativeUi]::SetThreadDpiAwarenessContext($previousDpiContext)
    }
    $smokeEnvironment = @{
        FLITZZIP_GUI_SMOKE_DESTINATION = $previousSmokeDestination
        FLITZZIP_GUI_SMOKE_FILE_SELECTION = $previousSmokeFiles
        FLITZZIP_GUI_SMOKE_FOLDER_SELECTION = $previousSmokeFolder
        FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS = $previousSmokeAutoClose
        FLITZZIP_GUI_SMOKE_CLOSE_FILE = $previousSmokeCloseFile
        FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT = $previousSmokeSettingsRedirect
        FLITZZIP_GUI_SMOKE_SUPPRESS_SHELL_OPEN = $previousSmokeShellOpen
    }
    foreach ($name in $smokeEnvironment.Keys) {
        Set-FlitzZipProcessEnvironmentValue -Name $name -Value $smokeEnvironment[$name] -Confirm:$false -WhatIf:$false
    }
    Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
    if ($cleanupFailure) {
        throw $cleanupFailure
    }
}
