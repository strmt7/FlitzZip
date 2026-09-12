# Purpose: Bound native build scheduling while honoring CMake's explicit parallelism override.
# Inputs: Requested is CMAKE_BUILD_PARALLEL_LEVEL; ProcessorCount is the available logical CPU count.
# Outputs: Returns 1..256 jobs; defaults to at most four and rejects invalid explicit overrides.
function Resolve-BuildParallelism {
    param(
        [AllowEmptyString()][string]$Requested = $env:CMAKE_BUILD_PARALLEL_LEVEL,
        [ValidateRange(1, 2147483647)][int]$ProcessorCount = [Environment]::ProcessorCount
    )

    if ([string]::IsNullOrEmpty($Requested)) {
        return [Math]::Min(4, $ProcessorCount)
    }
    if ($Requested -notmatch '\A[1-9][0-9]{0,2}\z' -or [int]$Requested -gt 256) {
        throw "CMAKE_BUILD_PARALLEL_LEVEL must be an integer from 1 through 256."
    }
    return [int]$Requested
}
