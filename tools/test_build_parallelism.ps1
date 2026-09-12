$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "build_parallelism.ps1")

foreach ($processors in @(1, 2, 4, 8, 64)) {
    if ((Resolve-BuildParallelism -Requested "" -ProcessorCount $processors) -ne [Math]::Min(4, $processors)) {
        throw "Default build parallelism must respect small hosts and cap shared-host load."
    }
}
foreach ($jobs in @(1, 2, 4, 16, 256)) {
    if ((Resolve-BuildParallelism -Requested ([string]$jobs) -ProcessorCount 8) -ne $jobs) {
        throw "Explicit build parallelism must be preserved."
    }
}
foreach ($invalid in @("0", "-1", "+4", "04", "257", "99999999999999999999", "1.5", "1e1", " ", "4 ", "4`n")) {
    $rejected = $false
    try {
        Resolve-BuildParallelism -Requested $invalid | Out-Null
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw "Invalid build parallelism must be rejected before invoking the build system."
    }
}
Write-Output "Build parallelism tests passed."
