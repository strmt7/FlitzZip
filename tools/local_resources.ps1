# Purpose: Read currently available host RAM without relying on a fixed machine configuration.
# Inputs: Windows operating-system counters; no other process is changed.
# Outputs: Returns available physical MiB, or throws rather than inventing capacity.
function Get-SuperZipAvailableMemoryMiB {
    $operatingSystem = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
    $available = [Math]::Floor([double]$operatingSystem.FreePhysicalMemory / 1024)
    if ($available -le 0) { throw 'Available physical RAM is unknown; do not start memory-intensive local work.' }
    return [long]$available
}

# Purpose: Reserve host headroom and share one planning policy across local analysis/build checks.
# Inputs: AvailableMiB is current free physical RAM; RequiredMiB is the work's minimum admission budget.
# Outputs: Returns at most half available RAM while preserving at least 2 GiB; throws if work cannot fit.
function Resolve-SuperZipLocalMemoryBudget {
    param(
        [ValidateRange(0, 2147483647)][long]$AvailableMiB,
        [ValidateRange(1, 2147483647)][long]$RequiredMiB = 2048
    )
    $headroom = [Math]::Max(2048, [Math]::Ceiling($AvailableMiB / 2.0))
    $budget = [Math]::Max(0, $AvailableMiB - $headroom)
    if ($budget -lt $RequiredMiB) {
        throw "Insufficient available RAM for local work: ${AvailableMiB} MiB available, ${budget} MiB admitted, ${RequiredMiB} MiB required."
    }
    return [long]$budget
}

# Purpose: Run this task's correctness/analysis work at cooperative Windows scheduling priority.
# Inputs: Action is trusted repository work; it must not contain performance timing.
# Outputs: Forwards action output/errors and restores only the calling process's original priority.
function Invoke-SuperZipBackgroundWork {
    param([Parameter(Mandatory = $true)][scriptblock]$Action)
    $process = [Diagnostics.Process]::GetCurrentProcess()
    $original = $process.PriorityClass
    try {
        if ($original -ne [Diagnostics.ProcessPriorityClass]::Idle) {
            $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
        }
        & $Action
    } finally {
        try { $process.PriorityClass = $original } finally { $process.Dispose() }
    }
}
