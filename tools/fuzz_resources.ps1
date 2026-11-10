. (Join-Path $PSScriptRoot 'local_resources.ps1')

# Query only one operation's unpredictable ownership label, retaining complete
# container identities. Failure to query the daemon never proves cleanup.
function Get-FlitzZipFuzzOwnedContainerId {
    param([Parameter(Mandatory = $true)][ValidatePattern('^[a-f0-9]{32}$')][string]$OwnerId)
    $identifiers = @(& docker ps --all --no-trunc --filter "label=io.flitzzip.fuzz-owner=$OwnerId" --format '{{.ID}}')
    if ($LASTEXITCODE -ne 0) { throw "Fuzz container ownership query failed ($LASTEXITCODE)." }
    foreach ($identifier in $identifiers) {
        if ($identifier -cnotmatch '^[a-f0-9]{64}$') { throw 'Fuzz ownership query returned an invalid container ID.' }
        $identifier
    }
}

# Revalidate each full ID against its actual daemon-owned name and label before
# removing it. The same operation may own its RAM probe and its fuzz workload.
# This is also the recovery boundary for a forcibly stopped launcher; callers
# retain OwnerId before launch and invoke it even when the launcher cannot unwind.
function Remove-FlitzZipFuzzOwnedContainer {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([Parameter(Mandatory = $true)][ValidatePattern('^[a-f0-9]{32}$')][string]$OwnerId)
    foreach ($identifier in @(Get-FlitzZipFuzzOwnedContainerId -OwnerId $OwnerId)) {
        # JSON projection avoids embedded template quotes lost by legacy Windows PowerShell's native argv transport.
        $proof = @(& docker inspect --format '{{.Name}}|{{json .Config.Labels}}' $identifier)
        if ($LASTEXITCODE -ne 0) {
            if (@(Get-FlitzZipFuzzOwnedContainerId -OwnerId $OwnerId) -notcontains $identifier) { continue }
            throw 'Cannot verify a remaining fuzz container owner.'
        }
        if ($proof.Count -ne 1 -or $proof[0] -cnotmatch "^/flitzzip-fuzz-$OwnerId-(probe|work)\|") {
            throw 'Fuzz container identity does not match its recorded operation.'
        }
        $separator = $proof[0].IndexOf('|')
        $labels = ConvertFrom-Json -InputObject $proof[0].Substring($separator + 1)
        if ($labels -isnot [pscustomobject] -or $labels.'io.flitzzip.fuzz-owner' -isnot [string] -or
            $labels.'io.flitzzip.fuzz-owner' -cne $OwnerId) {
            throw 'Fuzz container identity does not match its recorded operation.'
        }
        if (-not $PSCmdlet.ShouldProcess($identifier, 'Remove verified task-owned fuzz container')) {
            throw 'Owned fuzz cleanup was declined; its container remains unresolved.'
        }
        $null = & docker rm --force --volumes $identifier
        if ($LASTEXITCODE -ne 0 -and
            @(Get-FlitzZipFuzzOwnedContainerId -OwnerId $OwnerId) -contains $identifier) {
            throw 'Owned fuzz container removal failed.'
        }
    }
    if (@(Get-FlitzZipFuzzOwnedContainerId -OwnerId $OwnerId).Count -ne 0) {
        throw 'Owned fuzz containers remain after cleanup.'
    }
}

# Purpose: Query Linux Docker-host free RAM with a small, memory-contained trusted probe.
# Inputs: Image is the pinned fuzz image; RepoMount is the read-only repository mount.
# Outputs: Returns available MiB or throws on missing counters, limits, or Docker failures.
function Get-FlitzZipDockerAvailableMemoryMiB {
    param(
        [Parameter(Mandatory = $true)][string]$Image,
        [Parameter(Mandatory = $true)][string]$RepoMount,
        [ValidatePattern('^[a-f0-9]{32}$')][string]$OwnerId = [guid]::NewGuid().ToString('N')
    )
    if (@(Get-FlitzZipFuzzOwnedContainerId -OwnerId $OwnerId).Count -ne 0) {
        throw 'Fuzz operation owner is already in use; use recovery instead of starting new work.'
    }
    $response = & docker info --format '{{json .}}'
    if ($LASTEXITCODE -ne 0) { throw "Docker resource preflight failed with exit code $LASTEXITCODE." }
    $info = ConvertFrom-Json -InputObject ($response -join "`n")
    if ($info.OSType -ne 'linux' -or $info.MemoryLimit -isnot [bool] -or -not $info.MemoryLimit -or
        $info.SwapLimit -isnot [bool] -or -not $info.SwapLimit -or
        ($info.MemTotal -isnot [long] -and $info.MemTotal -isnot [int]) -or $info.MemTotal -le 0) {
        throw 'Local fuzzing requires Linux Docker memory and swap limit support with valid RAM counters.'
    }
    try {
        $response = & docker run --rm --name "flitzzip-fuzz-$OwnerId-probe" `
            --label "io.flitzzip.fuzz-owner=$OwnerId" --memory 64m --memory-swap 64m `
            -v "${RepoMount}:/src:ro" --entrypoint python3 $Image /src/tools/fuzz_memory.py probe
        $probeExit = $LASTEXITCODE
    } finally { Remove-FlitzZipFuzzOwnedContainer -OwnerId $OwnerId }
    if ($probeExit -ne 0) { throw "Docker RAM probe failed with exit code $probeExit." }
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
function Resolve-FlitzZipFuzzMemoryBudget {
    param(
        [ValidateRange(0, 2147483647)][long]$AvailableMiB,
        [ValidateRange(0, 2147483647)][long]$DockerAvailableMiB
    )
    $hostBudget = Resolve-FlitzZipLocalMemoryBudget -AvailableMiB $AvailableMiB
    $dockerBudget = Resolve-FlitzZipLocalMemoryBudget -AvailableMiB $DockerAvailableMiB
    return [long][Math]::Min($hostBudget, $dockerBudget)
}
