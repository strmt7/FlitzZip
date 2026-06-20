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

# Purpose: Fail fast when GUI source reintroduces previously rejected user-facing labels.
# Inputs: None; reads the repository app source tree.
# Outputs: Throws with a precise remediation message when a banned GUI string appears.
function Assert-GuiSourceContract {
    $appSources = Get-ChildItem -LiteralPath (Join-Path $repo "src\app") -File -Recurse -Include *.cpp, *.hpp, *.h, *.rc
    $sourceText = ($appSources | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
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
        if ($sourceText -cmatch $rule.Pattern) {
            throw $rule.Message
        }
    }
    foreach ($requiredLabel in @('L"1 week"', 'L"2 weeks"', 'L"1 month"')) {
        if (-not $sourceText.Contains($requiredLabel)) {
            throw "GUI log retention option missing required label $requiredLabel."
        }
    }
    if ($sourceText -cmatch 'found_process_sample') {
        throw "System GPU utilization graph must not prefer process-only PDH samples."
    }
    if ($sourceText -cmatch 'vram_span') {
        throw "System GPU graph must plot total GPU utilization, not VRAM history."
    }
    if (-not $sourceText.Contains('current_user_downloads_directory')) {
        throw "GUI destination defaults must resolve the current user's Downloads folder instead of process cwd."
    }
    if (-not $sourceText.Contains('queue_scrollbar_thumb_rect') -or -not $sourceText.Contains('WM_MOUSEWHEEL')) {
        throw "Queue overflow must keep a fixed header and expose a working scrollbar/wheel path."
    }
    if (-not $sourceText.Contains('std::array<PerformanceMonitorSample, 96> performance_history_')) {
        throw "System graph history cadence must not be changed without an explicit graph-cadence task."
    }
    if (-not $sourceText.Contains('Remove selected')) {
        throw "Queue must expose a Remove selected action when checked rows exist."
    }
    if (-not $sourceText.Contains('draw_extract_overwrite_prompt')) {
        throw "Extract Ask-before-overwriting policy must use a SuperZip-owned in-app modal."
    }
    if ($sourceText -cmatch 'MessageBoxW') {
        throw "Product confirmations must not use native MessageBoxW; use SuperZip-owned modal surfaces."
    }
    if (-not $sourceText.Contains('history_column_resize_separator_')) {
        throw "History table must keep Queue-equivalent column resizing support."
    }
    if ($sourceText.Contains('draw_interactive_hover_surface(dc, rect, interactive);')) {
        throw "Empty checkbox hover must not paint a row-sized placeholder surface."
    }
    if (-not $sourceText.Contains('const bool has_label = text != nullptr && text[0] != L''\0'';') -or
        -not $sourceText.Contains('const RECT paint_rect = has_label ? rect : hover_rect;')) {
        throw "Checkbox hover rendering must use a tight hover surface when there is no label text."
    }
    if (-not $sourceText.Contains('const int checkbox_target_size = scale(24);')) {
        throw "Queue checkbox hit/focus targets must stay tightly centered around the visible tick."
    }
    foreach ($requiredSecurityCall in @(
        'hash_path(output, IntegrityMode::Sha256)',
        'hash_path(archive, IntegrityMode::Sha256)',
        'hash_path(path, IntegrityMode::Sha256)',
        'scan_with_windows_defender(output, DefenderScanMode::FullPath)',
        'scan_with_windows_defender(archive, DefenderScanMode::FullPath)',
        'scan_with_windows_defender(path, DefenderScanMode::FullPath)'
    )) {
        if (-not $sourceText.Contains($requiredSecurityCall)) {
            throw "GUI security options must call the real integrity and Defender paths; missing $requiredSecurityCall."
        }
    }
    if ($sourceText -cmatch 'OPENFILENAMEW|GetOpenFileNameW|SHBrowseForFolderW|SHGetPathFromIDListW') {
        throw "Queue Add files/Add folder must use the modern shell picker without fixed legacy buffers."
    }
    if (-not $sourceText.Contains('IFileOpenDialog') -or -not $sourceText.Contains('append_queued_paths')) {
        throw "Queue Add files, Add folder, and drag/drop must share modern shell selection and queue append paths."
    }
    if (-not $sourceText.Contains('detect_archive_format_by_extension(path)') -or
        -not $sourceText.Contains('return L"Archive";')) {
        throw "Queue Type must classify supported archive files as Archive through extension-only detection."
    }
    foreach ($requiredExtractSource in @(
        'selected_extract_archive_paths',
        'Multiple selected archives',
        'L"Archive path"',
        'extraction_outputs_for_archives',
        'request.archives'
    )) {
        if (-not $sourceText.Contains($requiredExtractSource)) {
            throw "Extract page must support selected one-or-many archive extraction; missing $requiredExtractSource."
        }
    }
    if (-not $sourceText.Contains('constexpr std::size_t kFullGraphSampleCapacity = 96U') -or
        -not $sourceText.Contains('first_x')) {
        throw "Performance graphs must not stretch startup samples across the full plot."
    }
    if (-not $sourceText.Contains('constexpr UINT kTextTooltipDelayMs = 500')) {
        throw "Truncated text tooltip delay must remain 0.5 seconds."
    }
    if (-not $sourceText.Contains('performance_update_seconds = 3')) {
        throw "System Performance Monitor default refresh interval must remain 3 seconds."
    }
    if (-not $sourceText.Contains('sample_total_dedicated_vram_used_bytes') -or
        -not $sourceText.Contains('reconcile_vram_usage') -or
        -not (Get-Content -Raw -LiteralPath (Join-Path $repo 'tests/cpp/test_resource_usage.cpp')).Contains('vram_reconciliation_keeps_process_usage_under_total_usage')) {
        throw "VRAM total/dedicated display must use centralized, tested Windows dedicated-memory reconciliation."
    }
    if ($sourceText -cmatch 'L"Session"') {
        throw "History rows must use the real completion time, not the literal Session label."
    }
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
    $captures = @()
    $pageNames = @("Queue", "Compress", "Extract", "Security", "History", "System", "Settings", "About")
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
    Select-CompressFormatIndex -Handle $windowHandle -Dpi $windowDpi -Index 1
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
    Invoke-ClientClick -Handle $windowHandle -Dpi $windowDpi -DesignX 876 -DesignY 478
    Invoke-ClientKey -Handle $windowHandle -VirtualKey 0x1B
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
    $captures += Invoke-DropdownExercise -Handle $windowHandle -Dpi $windowDpi -Name "System-UpdateSpeed" -OpenX 1160 -OpenY 446 -SelectX 1160 -SelectY 554 -MenuLeft 1070 -MenuTop 470 -MenuRight 1220 -MenuBottom 606 -BasePath $basePath -Extension $extension
    $systemMonitorPath = "${basePath}-System-PerformanceMonitor$extension"
    $captures += Save-SuperZipScreenshot -Handle $windowHandle -Path $systemMonitorPath
    $offset = Get-ClientCaptureOffset -Handle $windowHandle
    Assert-DesignRectHasDetail -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 488 -Right 1138 -Bottom 696 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -MinUniqueColors 8
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 488 -Right 1138 -Bottom 696 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 63 -ExpectedGreen 181 -ExpectedBlue 221
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 488 -Right 1138 -Bottom 696 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 83 -ExpectedGreen 210 -ExpectedBlue 101
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 488 -Right 1138 -Bottom 696 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 237 -ExpectedGreen 179 -ExpectedBlue 61
    Assert-DesignRectHasColor -Path $systemMonitorPath -Dpi $windowDpi -Left 140 -Top 488 -Right 1138 -Bottom 696 -ClientOffsetX $offset.X -ClientOffsetY $offset.Y -ExpectedRed 214 -ExpectedGreen 34 -ExpectedBlue 45

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
    [Environment]::SetEnvironmentVariable("SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT", $previousSmokeSettingsRedirect, "Process")
    Remove-Item -LiteralPath $smokeCloseFile -Force -ErrorAction SilentlyContinue
    if ($cleanupFailure) {
        throw $cleanupFailure
    }
}
