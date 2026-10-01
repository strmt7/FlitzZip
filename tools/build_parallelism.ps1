. (Join-Path $PSScriptRoot 'local_resources.ps1')

# Purpose: Admit compiler workers from available RAM instead of an arbitrary fixed CPU cap.
# Inputs: Requested is CMAKE_BUILD_PARALLEL_LEVEL; ProcessorCount is logical CPUs; AvailableMiB is free RAM.
# Outputs: Returns memory-admitted jobs (2 GiB estimated per compiler); rejects invalid/over-budget overrides.
function Resolve-BuildParallelism {
    param(
        [AllowEmptyString()][string]$Requested = $env:CMAKE_BUILD_PARALLEL_LEVEL,
        [ValidateRange(1, 2147483647)][int]$ProcessorCount = [Environment]::ProcessorCount,
        [ValidateRange(0, 2147483647)][long]$AvailableMiB = (Get-SuperZipAvailableMemoryMiB)
    )
    $budget = Resolve-SuperZipLocalMemoryBudget -AvailableMiB $AvailableMiB
    $memoryJobs = [Math]::Floor($budget / 2048.0)
    if ([string]::IsNullOrEmpty($Requested)) {
        return [int][Math]::Min($memoryJobs, $ProcessorCount)
    }
    $parsed = 0
    if ($Requested -notmatch '\A[1-9][0-9]{0,9}\z' -or
        -not [int]::TryParse($Requested, [Globalization.NumberStyles]::None, [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        throw 'CMAKE_BUILD_PARALLEL_LEVEL must be a positive 32-bit integer.'
    }
    if ($parsed -gt $memoryJobs) {
        throw "CMAKE_BUILD_PARALLEL_LEVEL exceeds the current RAM budget (${budget} MiB at 2048 MiB per compiler)."
    }
    return $parsed
}

# Purpose: Keep CMake project scheduling and MSBuild compiler scheduling within the same admitted job count.
# Inputs: Jobs is RAM-admitted; Action invokes the trusted build without changing unrelated host work.
# Outputs: Forwards build output/errors and restores all overridden process-local environment variables.
function Invoke-SuperZipParallelBuild {
    param([ValidateRange(1, 2147483647)][int]$Jobs, [Parameter(Mandatory = $true)][scriptblock]$Action)
    $settings = @{
        CMAKE_BUILD_PARALLEL_LEVEL = [string]$Jobs; MultiProcMaxCount = [string]$Jobs
        UseMultiToolTask = 'true'; EnforceProcessCountAcrossBuilds = 'true'
    }
    $previous = @{}
    foreach ($name in $settings.Keys) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    try {
        foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process') }
        & $Action
    } finally {
        foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
