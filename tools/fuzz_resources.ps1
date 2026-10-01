. (Join-Path $PSScriptRoot 'local_resources.ps1')

# Purpose: Query Linux Docker-host free RAM with a small, memory-contained trusted probe.
# Inputs: Image is the pinned fuzz image; RepoMount is the read-only repository mount.
# Outputs: Returns available MiB or throws on missing counters, limits, or Docker failures.
function Get-SuperZipDockerAvailableMemoryMiB {
    param(
        [Parameter(Mandatory = $true)][string]$Image,
        [Parameter(Mandatory = $true)][string]$RepoMount
    )
    $response = & docker info --format '{{json .}}'
    if ($LASTEXITCODE -ne 0) { throw "Docker resource preflight failed with exit code $LASTEXITCODE." }
    $info = ConvertFrom-Json -InputObject ($response -join "`n")
    if ($info.OSType -ne 'linux' -or $info.MemoryLimit -isnot [bool] -or -not $info.MemoryLimit -or
        $info.SwapLimit -isnot [bool] -or -not $info.SwapLimit -or
        ($info.MemTotal -isnot [long] -and $info.MemTotal -isnot [int]) -or $info.MemTotal -le 0) {
        throw 'Local fuzzing requires Linux Docker memory and swap limit support with valid RAM counters.'
    }
    $response = & docker run --rm --memory 64m --memory-swap 64m `
        -v "${RepoMount}:/src:ro" --entrypoint python3 $Image /src/tools/fuzz_memory.py probe
    if ($LASTEXITCODE -ne 0) { throw "Docker RAM probe failed with exit code $LASTEXITCODE." }
    $text = ($response -join "`n").Trim()
    if ($text -notmatch '\A[1-9][0-9]*\z' -or $text.Length -gt 10) {
        throw 'Docker RAM probe did not return a valid available-memory counter.'
    }
    $available = [long]$text
    if ($available -gt [Math]::Floor([double]$info.MemTotal / 1MB) -or $available -gt [int]::MaxValue) {
        throw 'Docker RAM probe exceeds the reported host capacity.'
    }
    return $available
}

# Purpose: Admit one fuzz container against both Windows and Docker-host RAM headroom.
# Inputs: AvailableMiB and DockerAvailableMiB are current free physical-memory snapshots.
# Outputs: Returns the smaller shared-policy budget, or throws below the 2 GiB minimum.
function Resolve-SuperZipFuzzMemoryBudget {
    param(
        [ValidateRange(0, 2147483647)][long]$AvailableMiB,
        [ValidateRange(0, 2147483647)][long]$DockerAvailableMiB
    )
    $hostBudget = Resolve-SuperZipLocalMemoryBudget -AvailableMiB $AvailableMiB
    $dockerBudget = Resolve-SuperZipLocalMemoryBudget -AvailableMiB $DockerAvailableMiB
    return [long][Math]::Min($hostBudget, $dockerBudget)
}
