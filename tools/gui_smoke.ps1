param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$ScreenshotPath = "",
    [switch]$CompactOnly
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

# Purpose: Load GUI source text for source-contract assertions.
# Inputs: None; reads the repository app source tree.
# Outputs: Returns a single joined text buffer for policy checks.
function Get-GuiSourceText {
    $appSources = Get-ChildItem -LiteralPath (Join-Path $repo "src\app") -File -Recurse -Include *.cpp, *.hpp, *.h, *.rc
    return ($appSources | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
}

# Purpose: Fail fast when GUI source reintroduces previously rejected user-facing labels.
# Inputs: `SourceText` is the joined GUI source text.
# Outputs: Throws with a precise remediation message when a banned GUI string appears.
function Assert-GuiBlockedText {
    param([Parameter(Mandatory = $true)][string]$SourceText)

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
        if ($SourceText -cmatch $rule.Pattern) {
            throw $rule.Message
        }
    }
    foreach ($requiredLabel in @('L"1 week"', 'L"2 weeks"', 'L"1 month"')) {
        if (-not $SourceText.Contains($requiredLabel)) {
            throw "GUI log retention option missing required label $requiredLabel."
        }
    }
    if ($SourceText -cmatch 'L"Session"') {
        throw "History rows must use the real completion time, not the literal Session label."
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
        throw "System I/O monitor must not regress to SuperZip process-only I/O counters."
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
        'directory_options::skip_permission_denied'
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
        throw "Extract Ask-before-overwriting policy must use a SuperZip-owned in-app modal."
    }
    if ($SourceText -cmatch 'MessageBoxW') {
        throw "Product confirmations must not use native MessageBoxW; use SuperZip-owned modal surfaces."
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
    if (-not $SourceText.Contains('".suzip",   ".zip"') -or
        -not $SourceText.Contains('constexpr int kDefaultCompressionFormatIndex = 0')) {
        throw "Compress Format must list .suzip first and use it as the default row."
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
        throw "SuperZip-owned UI must consume text-copy accelerators instead of exposing copyable text."
    }
    if ($SourceText -match 'OpenClipboard|SetClipboardData|GetClipboardData|CreateWindowExW[^\r\n]*(EDIT|RICHEDIT)|CreateWindowW[^\r\n]*(EDIT|RICHEDIT)') {
        throw "SuperZip-owned UI must not expose selectable text controls or clipboard APIs."
    }
    if (-not $SourceText.Contains('license_notices_dialog_visible') -or
        -not $SourceText.Contains('superzip_license_notices.hpp')) {
        throw "About Licenses must be backed by the generated license-notices header and modal state."
    }
    if (-not $SourceText.Contains('license_notices_tab_rects') -or
        -not $SourceText.Contains('select_license_notices_tab')) {
        throw "About Licenses must keep SuperZip and Other tabs for readable non-copyable notices."
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
        throw "SuperZip-owned secondary windows must use the same full-width top red line as the main interface."
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
        -not $SourceText.Contains('reconcile_vram_usage') -or
        -not (Get-Content -Raw -LiteralPath (Join-Path $repo 'tests/cpp/test_resource_usage.cpp')).Contains('vram_reconciliation_keeps_process_usage_under_total_usage')) {
        throw "VRAM total/dedicated display must use centralized, tested Windows dedicated-memory reconciliation."
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
# Inputs: `Handle`/`Dpi` identify the SuperZip window and `Index` is the zero-based Compress format row.
# Outputs: Opens the dropdown, moves to the requested row, selects it, and waits for repaint.
function Select-CompressFormatIndex {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [int]$Index
    )
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 500 -DesignY 224
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

Import-Module (Join-Path $PSScriptRoot "SuperZip.GuiSmoke.Ui.psm1") -Force

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
# Inputs: Path is the smoke log, PreviousLength is its prior byte length, and Message is expected row text.
# Outputs: Returns after the expected new row, or throws after five seconds without it.
function Wait-GuiLogEvent {
    param([string]$Path, [long]$PreviousLength, [string]$Message)

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 5) {
        if ((Get-Item -LiteralPath $Path).Length -gt $PreviousLength) {
            $lastLine = Get-Content -LiteralPath $Path -Tail 1
            if ($lastLine -like "*$Message*") { return }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "GUI command did not report '$Message' within five seconds."
}

# Purpose: Verify failed Apply and Restore Defaults keep the last successfully saved settings active.
# Inputs: Handle/Dpi identify the smoke window; Path is its redirected temporary settings file.
# Outputs: Exercises both save failures and throws if navigation promotes unsaved settings.
function Assert-SettingsSaveFailureRollback {
    param(
        [IntPtr]$Handle,
        [int]$Dpi,
        [string]$Path
    )

    $original = Get-Content -Raw -LiteralPath $Path
    $logPath = Join-Path (Split-Path -Parent $Path) "superzip.log"
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
    $logPath = Join-Path (Split-Path -Parent $SettingsPath) "superzip.log"
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
            [void][SuperZipNativeUi]::PostMessage($Handle, 0x0200, [IntPtr]::Zero,
                (ConvertTo-MouseLParam -X 0 -Y 0))
            Start-Sleep -Milliseconds 250
            $path = "${BasePath}-History-$method-$direction$Extension"
            Save-SuperZipScreenshot -Handle $Handle -Path $path
            $top = if ($direction -eq 'Bottom') { 570 } else { 234 }
            Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left 1156 -Top $top -Right 1160 -Bottom ($top + 4) -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 64 -ExpectedGreen 83 -ExpectedBlue 90 -Tolerance 12 -MinPixels 8
        }
    }
    Write-Information "History overflow endpoints passed: wheel, track click, and thumb drag." -InformationAction Continue
}

# Purpose: Resize only the owned smoke window using its actual native frame dimensions.
# Inputs: Handle/Dpi identify the test window; Width/Height are required client DIPs.
# Outputs: Returns prior outer dimensions and throws unless the exact requested client size is reached.
function Invoke-SmokeClientResize {
    param([IntPtr]$Handle, [int]$Dpi, [int]$Width, [int]$Height)

    $window = New-Object SuperZipNativeUi+RECT
    $client = New-Object SuperZipNativeUi+RECT
    if (-not [SuperZipNativeUi]::GetWindowRect($Handle, [ref]$window) -or
        -not [SuperZipNativeUi]::GetClientRect($Handle, [ref]$client)) { throw 'Cannot inspect smoke viewport.' }
    $old = @{ Width = $window.Right - $window.Left; Height = $window.Bottom - $window.Top }
    $physicalWidth = [int][Math]::Round($Width * $Dpi / 96.0)
    $physicalHeight = [int][Math]::Round($Height * $Dpi / 96.0)
    if (-not [SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0,
        ($physicalWidth + $old.Width - $client.Right), ($physicalHeight + $old.Height - $client.Bottom), 0x0016)) {
        throw 'Cannot resize the owned smoke window.'
    }
    if (-not [SuperZipNativeUi]::GetClientRect($Handle, [ref]$client) -or
        $client.Right -ne $physicalWidth -or $client.Bottom -ne $physicalHeight) { throw 'Client dimensions mismatch.' }
    return $old
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
            if ([SuperZipNativeUi]::SendMessageTimeout($Handle, $notification[0], [IntPtr]$notification[1], [IntPtr]::Zero,
                0x0002, 5000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Window fitting notification timed out.' }
        }
        $window = New-Object SuperZipNativeUi+RECT
        $client = New-Object SuperZipNativeUi+RECT
        $info = New-Object SuperZipNativeUi+MONITORINFO
        $info.Size = [Runtime.InteropServices.Marshal]::SizeOf($info)
        if (-not [SuperZipNativeUi]::GetWindowRect($Handle, [ref]$window) -or
            -not [SuperZipNativeUi]::GetClientRect($Handle, [ref]$client) -or
            -not [SuperZipNativeUi]::GetMonitorInfo([SuperZipNativeUi]::MonitorFromWindow($Handle, 2), [ref]$info)) {
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

# Purpose: Exercise the compact Licenses modal using native buttons, tabs, and scrolling.
# Inputs: Handle/Dpi identify a compact About page; BasePath/Extension name screenshots.
# Outputs: Returns validated modal captures; closes through the real Close button before later form checks.
function Assert-CompactLicenseDialog {
    param([IntPtr]$Handle, [int]$Dpi, [string]$BasePath, [string]$Extension, [string]$SettingsPath)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'superzip.log'
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 434 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'License notices opened'
    $offset = Get-ClientCaptureOffset -Handle $Handle
    foreach ($tab in 0..1) {
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX (220 + 136 * $tab) -DesignY 176 -Synchronous
        $path = "${BasePath}-Compact-Licenses-$tab$Extension"
        Save-SuperZipScreenshot -Handle $Handle -Path $path -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        Assert-DesignRectHasColor -Path $path -Dpi $Dpi -Left (165 + 136 * $tab) -Top 159 -Right (293 + 136 * $tab) -Bottom 193 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
        Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left 175 -Top 209 -Right 867 -Bottom 443 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    }
    Invoke-ClientKey -Handle $Handle -VirtualKey 0x23
    Start-Sleep -Milliseconds 200
    Save-SuperZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-Licenses-End$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
    $length = (Get-Item -LiteralPath $logPath).Length
    Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 826 -DesignY 481 -Synchronous
    Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'License notices closed'
    $closedPath = "${BasePath}-Compact-Licenses-Closed$Extension"
    Save-SuperZipScreenshot -Handle $Handle -Path $closedPath -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
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
        Save-SuperZipScreenshot -Handle $Handle -Path $path -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        $offset = Get-ClientCaptureOffset -Handle $Handle
        Assert-DesignRectHasDetail -Path $path -Dpi $Dpi -Left 230 -Top 186 -Right 810 -Bottom 425 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 756 -DesignY 398 -Synchronous
        $closedPath = "${BasePath}-Compact-OverwriteCancelled$Extension"
        Save-SuperZipScreenshot -Handle $Handle -Path $closedPath -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        Assert-DesignRectHasDetail -Path $closedPath -Dpi $Dpi -Left 116 -Top 74 -Right 350 -Bottom 110 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    } finally {
        if (-not [SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0, $old.Width, $old.Height, 0x0016)) {
            throw 'Cannot restore the smoke window after the overwrite prompt test.'
        }
    }
}

# Purpose: Verify compact popup scrolling through real selection and persistence.
# Inputs: Handle/Dpi identify a 960x600-DIP smoke window; SettingsPath is isolated storage; BasePath names screenshots.
# Outputs: Returns menu captures and fails when wheel, scroll arrows, or keyboard cannot select the last format.
function Assert-CompactFormatMenu {
    param([IntPtr]$Handle, [int]$Dpi, [string]$SettingsPath, [string]$BasePath, [string]$Extension)

    $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'superzip.log'
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
        Save-SuperZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-Format-$method$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        if ($method -eq 'Keyboard') {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        } else {
            Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 515 -Synchronous
        }
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 6 -Synchronous
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Assert-SettingsValue -Path $SettingsPath -Name 'compressionFormatIndex' -Expected 18
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
            Save-SuperZipScreenshot -Handle $Handle -Path "${BasePath}-Compact-$($names[$index])$Extension" -ExpectedDesignWidth 960 -ExpectedDesignHeight 600
        }
        Assert-CompactLicenseDialog -Handle $Handle -Dpi $Dpi -BasePath $BasePath -Extension $Extension -SettingsPath $SettingsPath
        Assert-CompactFormatMenu -Handle $Handle -Dpi $Dpi -SettingsPath $SettingsPath -BasePath $BasePath -Extension $Extension
        $original = Get-Content -Raw -LiteralPath $SettingsPath | ConvertFrom-Json
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 440 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        $logPath = Join-Path (Split-Path -Parent $SettingsPath) 'superzip.log'
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Assert-SettingsValue -Path $SettingsPath -Name 'confirmBeforeDeleting' -Expected (-not $original.confirmBeforeDeleting)
        Assert-SettingsValue -Path $SettingsPath -Name 'logRetentionIndex' -Expected 0
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 175 -DesignY 227 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 650 -DesignY 440 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($row = 0; $row -lt $baseline.logRetentionIndex; ++$row) {
            Invoke-ClientKey -Handle $Handle -VirtualKey 0x28
        }
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x0D
        $length = (Get-Item -LiteralPath $logPath).Length
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 875 -DesignY 508 -Synchronous
        Wait-GuiLogEvent -Path $logPath -PreviousLength $length -Message 'Settings applied and saved'
        Invoke-SidebarClick -Handle $Handle -Dpi $Dpi -PageIndex 1 -Synchronous
        Invoke-ClientClick -Handle $Handle -Dpi $Dpi -DesignX 300 -DesignY 218 -Synchronous
        Invoke-ClientKey -Handle $Handle -VirtualKey 0x24
        for ($row = 0; $row -lt $baseline.compressionFormatIndex; ++$row) {
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
        if (-not [SuperZipNativeUi]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0, $old.Width, $old.Height, 0x0016)) {
            throw 'Cannot restore the owned smoke window dimensions.'
        }
    }
}

$smokeRoot = Join-Path $repo "out\gui-smoke-work"
$smokeDestination = Join-Path $smokeRoot "SuperZip-destination"
New-Item -ItemType Directory -Force -Path $smokeRoot | Out-Null
$smokeInput = Join-Path $smokeRoot "drag-drop-input.txt"
$smokeInputTwo = Join-Path $smokeRoot "drag-drop-input-two.txt"
$smokeFolder = Join-Path $smokeRoot "folder-input"
$smokeArchive = Join-Path $smokeRoot "valid-input.suzip"
$smokeArchiveTwo = Join-Path $smokeRoot "valid-input-two.suzip"
$badArchive = Join-Path $smokeRoot "invalid-input.suzip"
$overflowFiles = @()
$bulkDropFiles = @()
$smokeCloseFile = Join-Path $smokeRoot "close.request"
$smokeSettingsDir = Join-Path ([System.IO.Path]::GetTempPath()) "SuperZip"
$smokeSettingsFile = Join-Path $smokeSettingsDir "gui-smoke-settings.json"
New-Item -ItemType Directory -Force -Path $smokeSettingsDir | Out-Null
Remove-Item -LiteralPath $smokeDestination -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $smokeDestination | Out-Null
Set-Content -LiteralPath $smokeInput -Value "SuperZip GUI smoke input" -NoNewline
Set-Content -LiteralPath $smokeInputTwo -Value "Second SuperZip GUI smoke input" -NoNewline
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
Set-Content -LiteralPath $badArchive -Value "not a valid SuperZip archive" -NoNewline
Remove-Item -LiteralPath $smokeArchive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeArchiveTwo -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $smokeSettingsFile -Force -ErrorAction SilentlyContinue
& (Join-Path $repo "build\$Configuration\superzip_cli.exe") compress --format suzip --output $smokeArchive --force-cpu $smokeInput | Out-Null
& (Join-Path $repo "build\$Configuration\superzip_cli.exe") compress --format suzip --output $smokeArchiveTwo --force-cpu $smokeInputTwo | Out-Null
if (-not (Test-Path -LiteralPath $smokeArchive)) {
    throw "Could not create valid SUZIP archive for GUI extract smoke."
}
if (-not (Test-Path -LiteralPath $smokeArchiveTwo)) {
    throw "Could not create second valid SUZIP archive for GUI multi-extract smoke."
}
$previousSmokeDestination = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", "Process")
$previousSmokeFiles = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", "Process")
$previousSmokeFolder = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", "Process")
$previousSmokeAutoClose = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", "Process")
$previousSmokeCloseFile = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_CLOSE_FILE", "Process")
$previousSmokeSettingsRedirect = [Environment]::GetEnvironmentVariable("SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT", "Process")
$smokeAutoCloseMs = 300000
if ($smokeAutoCloseMs -lt 240000) {
    throw "GUI smoke auto-close timeout must leave enough time for the full tab/control pass."
}
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_DESTINATION", $smokeDestination, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FILE_SELECTION", $queuePickerSelection, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", (Resolve-Path -LiteralPath $smokeFolder).Path, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", [string]$smokeAutoCloseMs, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_CLOSE_FILE", $smokeCloseFile, "Process")
[Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT", "1", "Process")

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
    Assert-WindowWorkAreaFit -Handle $windowHandle -Dpi $windowDpi
    Assert-WindowWorkAreaFit -Handle $windowHandle -Dpi $windowDpi -ExerciseEvents
    [void](Invoke-SmokeClientResize -Handle $windowHandle -Dpi $windowDpi -Width 1200 -Height 760)
    $captures = @()
    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "System", "Settings", "About")
    $basePath = Join-Path (Split-Path -Parent $ScreenshotPath) ([System.IO.Path]::GetFileNameWithoutExtension($ScreenshotPath))
    $extension = [System.IO.Path]::GetExtension($ScreenshotPath)
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $ScreenshotPath

    if ($CompactOnly) {
        $captures += Assert-CompactFormLayout -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
        $captures | Format-Table -AutoSize
        return
    }

    # Queue header actions: exercise Add files, Add folder, and Clear without modal dialogs.
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 918 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1032 -DesignY 91
    Start-Sleep -Milliseconds 150
    $pickerQueuePath = "${basePath}-Queue-AfterPickers$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $pickerQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $pickerQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Assert-DesignRectHasDetail -Path $pickerQueuePath -Dpi $windowDpi -Left 1148 -Top 170 -Right 1166 -Bottom 640 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 3
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 505 -DesignY 91
    Start-Sleep -Milliseconds 180
    $removeSelectedQueuePath = "${basePath}-Queue-AfterRemoveSelected$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $removeSelectedQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $removeSelectedQueuePath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 918 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1032 -DesignY 91
    Start-Sleep -Milliseconds 150
    $pickerReloadedQueuePath = "${basePath}-Queue-AfterPickersReloaded$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $pickerReloadedQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $pickerReloadedQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-ClientDrag -Handle $windowHandle -Dpi $windowDpi -StartX 1158 -StartY 186 -EndX 1158 -EndY 344
    Start-Sleep -Milliseconds 180
    $scrolledQueuePath = "${basePath}-Queue-AfterScrollbarDrag$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $scrolledQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $scrolledQueuePath -Dpi $windowDpi -Left 126 -Top 132 -Right 620 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    $emptyQueuePath = "${basePath}-Queue-EmptyDropZone$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $emptyQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $emptyQueuePath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y

    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths $bulkDropFiles
    Start-Sleep -Milliseconds 450
    $bulkDropQueuePath = "${basePath}-Queue-AfterBulkDragDrop$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $bulkDropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $bulkDropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 520 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Assert-DesignRectHasDetail -Path $bulkDropQueuePath -Dpi $windowDpi -Left 1148 -Top 170 -Right 1166 -Bottom 640 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 3
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    $emptyAfterBulkDropPath = "${basePath}-Queue-AfterBulkDropClear$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $emptyAfterBulkDropPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-QueueEmptyMessageCentered -Path $emptyAfterBulkDropPath -Dpi $windowDpi -ClientOffsetX $offset.X -ClientOffsetY $offset.Y

    # Queue: exercise drag/drop and row selection only. Destination, level, and Start belong to Compress/Extract.
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $smokeInput).Path)
    Start-Sleep -Milliseconds 350
    $dropQueuePath = "${basePath}-Queue-AfterDragDrop$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $dropQueuePath
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
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $queueColumnPath
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
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Level" -OpenX 820 -OpenY 224 -SelectX 820 -SelectY 390 -MenuLeft 657 -MenuTop 252 -MenuRight 1158 -MenuBottom 414 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-Method" -OpenX 500 -OpenY 294 -SelectX 500 -SelectY 370 -MenuLeft 116 -MenuTop 322 -MenuRight 617 -MenuBottom 388 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Compress-BlockSize" -OpenX 820 -OpenY 294 -SelectX 820 -SelectY 498 -MenuLeft 657 -MenuTop 322 -MenuRight 1158 -MenuBottom 548 -BasePath $basePath -Extension $extension
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
    $expectedZstd = Join-Path $smokeDestination "SuperZip-output.zst"
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

    $expectedTarZstd = Join-Path $smokeDestination "SuperZip-output.tar.zst"
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

    $expectedTarGz = Join-Path $smokeDestination "SuperZip-output.tar.gz"
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

    $expectedCpioGz = Join-Path $smokeDestination "SuperZip-output.cpgz"
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
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $archiveDropQueuePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $archiveDropQueuePath -Dpi $windowDpi -Left 126 -Top 168 -Right 560 -Bottom 204 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 4
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 820 -DesignY 154
    Start-Sleep -Milliseconds 120
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 520 -DesignY 227
    Start-Sleep -Milliseconds 120
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 300 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension
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

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Remove-Item -LiteralPath $extractOutput -Recurse -Force -ErrorAction SilentlyContinue
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
    $multiSecond = Join-Path $extractOutput "valid-input-two\drag-drop-input-two.txt"
    foreach ($attempt in 1..50) {
        if ((Test-Path -LiteralPath $multiFirst) -and (Test-Path -LiteralPath $multiSecond)) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $multiFirst) -or -not (Test-Path -LiteralPath $multiSecond)) {
        throw "GUI multi-archive extraction did not restore both expected files under $extractOutput."
    }

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Invoke-FileDrop -Handle $windowHandle -Dpi $windowDpi -Paths @((Resolve-Path -LiteralPath $smokeArchive).Path)
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 2
    Start-Sleep -Milliseconds 250
    Remove-Item -LiteralPath $extractOutput -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $extractOutput | Out-Null
    Set-Content -LiteralPath (Join-Path $extractOutput "existing-output.txt") -Value "Existing extraction output" -NoNewline
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite-Ask" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 266 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1090 -DesignY 666
    Start-Sleep -Milliseconds 250
    $overwritePromptPath = "${basePath}-Extract-OverwritePrompt$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $overwritePromptPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $overwritePromptPath -Dpi $windowDpi -Left 290 -Top 250 -Right 910 -Bottom 510 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    $captures += Assert-CompactOverwritePrompt -Handle $windowHandle -Dpi $windowDpi -BasePath $basePath -Extension $extension
    if (@(Get-ChildItem -LiteralPath $extractOutput -File).Count -ne 1 -or
        (Get-Content -Raw -LiteralPath (Join-Path $extractOutput 'existing-output.txt')) -cne 'Existing extraction output') {
        throw 'Cancelling compact overwrite confirmation modified the destination.'
    }
    Start-Sleep -Milliseconds 300
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Extract-Overwrite-Revert" -OpenX 900 -OpenY 225 -SelectX 900 -SelectY 300 -MenuLeft 657 -MenuTop 250 -MenuRight 1158 -MenuBottom 318 -BasePath $basePath -Extension $extension

    # Extract failure: queue a corrupt SUZIP and verify History exposes a real Failure row.
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 150
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1134 -DesignY 91
    Start-Sleep -Milliseconds 150
    Remove-Item -LiteralPath (Join-Path $smokeRoot "SuperZip-extracted") -Recurse -Force -ErrorAction SilentlyContinue
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

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 4
    Start-Sleep -Milliseconds 250
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Operation" -OpenX 220 -OpenY 145 -SelectX 220 -SelectY 246 -MenuLeft 116 -MenuTop 170 -MenuRight 336 -MenuBottom 300 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Status-Success" -OpenX 390 -OpenY 145 -SelectX 390 -SelectY 214 -MenuLeft 354 -MenuTop 170 -MenuRight 574 -MenuBottom 268 -BasePath $basePath -Extension $extension
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "History-Status-Failure" -OpenX 390 -OpenY 145 -SelectX 390 -SelectY 246 -MenuLeft 354 -MenuTop 170 -MenuRight 574 -MenuBottom 268 -BasePath $basePath -Extension $extension
    $historyFailurePath = "${basePath}-History-FailureRows$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $historyFailurePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasColor -Path $historyFailurePath -Dpi $windowDpi -Left 700 -Top 210 -Right 1138 -Bottom 470 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 236 -ExpectedGreen 73 -ExpectedBlue 73 -Tolerance 42 -MinPixels 4
    Invoke-ClientWheel -Handle $windowHandle -Dpi $windowDpi -DesignX 650 -DesignY 670 -Delta -120
    Start-Sleep -Milliseconds 80
    $historyDetailsWheelPath = "${basePath}-History-Details-SmoothWheel$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $historyDetailsWheelPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $historyDetailsWheelPath -Dpi $windowDpi -Left 116 -Top 628 -Right 1138 -Bottom 716 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientDrag -Handle $windowHandle -Dpi $windowDpi -StartX 430 -StartY 205 -EndX 468 -EndY 205
    Start-Sleep -Milliseconds 180
    $historyResizePath = "${basePath}-History-AfterColumnResize$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $historyResizePath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $historyResizePath -Dpi $windowDpi -Left 116 -Top 200 -Right 1138 -Bottom 260 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 5
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1100 -DesignY 90
    Start-Sleep -Milliseconds 250

    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 5
    Start-Sleep -Milliseconds 250
    $systemLogPath = Join-Path (Split-Path -Parent $smokeSettingsFile) "superzip.log"
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
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $systemMonitorPath
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
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-MemoryPolicy" -OpenX 700 -OpenY 247 -SelectX 700 -SelectY 318 -MenuLeft 622 -MenuTop 274 -MenuRight 887 -MenuBottom 372 -BasePath $basePath -Extension $extension
    foreach ($rowY in @(424, 456, 488)) {
        $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogLevel-$rowY" -OpenX 700 -OpenY 384 -SelectX 700 -SelectY $rowY -MenuLeft 622 -MenuTop 412 -MenuRight 887 -MenuBottom 510 -BasePath $basePath -Extension $extension
    }
    foreach ($rowY in @(482, 514, 546)) {
        $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogRetention-$rowY" -OpenX 700 -OpenY 442 -SelectX 700 -SelectY $rowY -MenuLeft 622 -MenuTop 470 -MenuRight 887 -MenuBottom 568 -BasePath $basePath -Extension $extension
    }
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1110 -DesignY 666
    Start-Sleep -Milliseconds 250
    Assert-SettingsValue -Path $smokeSettingsFile -Name "memoryPolicyIndex" -Expected 1
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logLevelIndex" -Expected 2
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logRetentionIndex" -Expected 2

    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "Settings-LogLevel-UnappliedWarning" -OpenX 700 -OpenY 384 -SelectX 700 -SelectY 456 -MenuLeft 622 -MenuTop 412 -MenuRight 887 -MenuBottom 510 -BasePath $basePath -Extension $extension
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 0
    Start-Sleep -Milliseconds 250
    Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex 6
    Start-Sleep -Milliseconds 250
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1110 -DesignY 666
    Start-Sleep -Milliseconds 250
    Assert-SettingsValue -Path $smokeSettingsFile -Name "logLevelIndex" -Expected 2

    Assert-SettingsSaveFailureRollback -Handle $windowHandle -Dpi $windowDpi -Path $smokeSettingsFile

    $captures += Assert-HistoryScrollEndpoint -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension
    $captures += Assert-CompactFormLayout -Handle $windowHandle -Dpi $windowDpi -SettingsPath $smokeSettingsFile -BasePath $basePath -Extension $extension

    for ($index = 0; $index -lt $pageNames.Count; ++$index) {
        Invoke-SidebarClick -Handle $windowHandle -Dpi $windowDpi -PageIndex $index
        Start-Sleep -Milliseconds 300
        $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path "${basePath}-$($pageNames[$index])$extension"
    }
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 1070 -DesignY 594
    Start-Sleep -Milliseconds 180
    $licenseDialogPath = "${basePath}-About-Licenses$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $licenseDialogPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $licenseDialogPath -Dpi $windowDpi -Left 260 -Top 120 -Right 940 -Bottom 650 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Assert-DesignRectHasColor -Path $licenseDialogPath -Dpi $windowDpi -Left 286 -Top 170 -Right 400 -Bottom 212 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45 -Tolerance 42 -MinPixels 20
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 450 -DesignY 198
    Start-Sleep -Milliseconds 120
    $licenseOtherDialogPath = "${basePath}-About-Licenses-Other$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $licenseOtherDialogPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $licenseOtherDialogPath -Dpi $windowDpi -Left 260 -Top 120 -Right 940 -Bottom 650 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Invoke-ClientWheel -Handle $windowHandle -Dpi $windowDpi -DesignX 610 -DesignY 420 -Delta -120
    Start-Sleep -Milliseconds 80
    $licenseSmoothWheelPath = "${basePath}-About-Licenses-SmoothWheel$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $licenseSmoothWheelPath
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
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT", $previousSmokeSettingsRedirect, "Process")
    Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
    if ($cleanupFailure) {
        throw $cleanupFailure
    }
}
