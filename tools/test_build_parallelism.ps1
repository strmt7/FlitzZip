$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "build_parallelism.ps1")

foreach ($processors in @(1, 2, 4, 8, 64)) {
    if ((Resolve-BuildParallelism -Requested "" -ProcessorCount $processors -AvailableMiB 1048576) -ne $processors) {
        throw 'Sufficient RAM must allow every logical processor without a fixed CPU cap.'
    }
}
foreach ($jobs in @(1, 2, 4, 16, 256)) {
    if ((Resolve-BuildParallelism -Requested ([string]$jobs) -ProcessorCount 8 -AvailableMiB 1048576) -ne $jobs) {
        throw "Explicit build parallelism must be preserved."
    }
}
if ((Resolve-BuildParallelism -Requested '512' -ProcessorCount 512 -AvailableMiB 4194304) -ne 512 -or
    (Resolve-BuildParallelism -Requested '' -ProcessorCount 512 -AvailableMiB 4194304) -ne 512) {
    throw 'Large-memory hosts must not encounter an artificial 256-worker ceiling.'
}
foreach ($invalid in @("0", "-1", "+4", "04", "2147483648", "99999999999999999999", "1.5", "1e1", " ", "4 ", "4`n")) {
    $rejected = $false
    try {
        Resolve-BuildParallelism -Requested $invalid -AvailableMiB 1048576 | Out-Null
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw "Invalid build parallelism must be rejected before invoking the build system."
    }
}
foreach ($case in @(@(4096, 1), @(8192, 2), @(16384, 4), @(32768, 8), @(65536, 16))) {
    if ((Resolve-BuildParallelism -Requested '' -ProcessorCount 64 -AvailableMiB $case[0]) -ne $case[1]) {
        throw 'Compiler admission must follow current RAM, not host identity or a fixed job count.'
    }
}
foreach ($available in @(0, 2048, 4095)) {
    $rejected = $false
    try { Resolve-BuildParallelism -Requested '' -AvailableMiB $available | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Insufficient or unknown RAM must reject memory-intensive build admission.' }
}
$rejected = $false
try { Resolve-BuildParallelism -Requested '2' -AvailableMiB 4096 | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Explicit worker overrides must not bypass RAM admission.' }
$settings = @('CMAKE_BUILD_PARALLEL_LEVEL', 'MultiProcMaxCount', 'UseMultiToolTask', 'EnforceProcessCountAcrossBuilds')
$previous = @{}
foreach ($name in $settings) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
foreach ($failAction in @($false, $true)) {
    $caught = $false
    try {
        Invoke-SuperZipParallelBuild -Jobs 3 -Action {
            if ($env:CMAKE_BUILD_PARALLEL_LEVEL -ne '3' -or $env:MultiProcMaxCount -ne '3' -or
                $env:UseMultiToolTask -ne 'true' -or $env:EnforceProcessCountAcrossBuilds -ne 'true') {
                throw 'Build child scheduling must share aggregate compiler admission.'
            }
            if ($failAction) { throw 'test: build failure' }
        }
    } catch {
        if ($_.Exception.Message -ne 'test: build failure') { throw }
        $caught = $true
    }
    if ($caught -ne $failAction) { throw 'Build invocation must preserve success/failure.' }
    foreach ($name in $settings) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) {
            throw "Build invocation did not restore $name."
        }
    }
}
$process = [Diagnostics.Process]::GetCurrentProcess()
$originalPriority = $process.PriorityClass
try {
    $observed = Invoke-SuperZipBackgroundWork { [Diagnostics.Process]::GetCurrentProcess().PriorityClass }
    $expectedPriority = if ($originalPriority -eq 'Idle') { 'Idle' } else { 'BelowNormal' }
    if ($observed -ne $expectedPriority -or $process.PriorityClass -ne $originalPriority) {
        throw 'Background work must use cooperative priority and restore the caller.'
    }
    $childPriority = Invoke-SuperZipBackgroundWork {
        & powershell -NoProfile -Command '[Diagnostics.Process]::GetCurrentProcess().PriorityClass.ToString()'
    }
    if ($childPriority -ne $expectedPriority) { throw 'Native correctness children must inherit cooperative priority.' }
    Invoke-SuperZipBackgroundWork { & powershell -NoProfile -Command 'exit 19' }
    if ($LASTEXITCODE -ne 19) { throw 'Cooperative scheduling must preserve native exit status.' }
    $global:LASTEXITCODE = 0
    $caught = $false
    try { Invoke-SuperZipBackgroundWork { throw 'test: background failure' } } catch {
        $caught = ($_.Exception.Message -eq 'test: background failure')
    }
    if (-not $caught -or $process.PriorityClass -ne $originalPriority) {
        throw 'Background work must preserve action failures and restore priority during unwinding.'
    }
} finally { $process.Dispose() }
Write-Output "Build parallelism tests passed."
