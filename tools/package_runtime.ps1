# Purpose: Validate installed runtime bytes independently from optional GPU hardware availability.
# Inputs: PackageRoot is an installed/staged payload; HipEnabled is its configured product capability.
# Outputs: Throws for missing, redirected, duplicate, unexpected or checksum-mismatched packaged runtimes.
function Assert-SuperZipPackagedRuntime {
    param([Parameter(Mandatory = $true)][string]$PackageRoot, [bool]$HipEnabled)

    foreach ($directory in @($PackageRoot, (Join-Path $PackageRoot 'bin'))) {
        $item = Get-Item -LiteralPath $directory -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Packaged runtime directory is redirected or invalid.'
        }
    }
    $manifestPath = Join-Path $PackageRoot 'superzip-runtime-dependencies.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.product -ne 'SuperZip' -or $manifest.gpu_backend.enabled -ne $HipEnabled) {
        throw 'Packaged runtime manifest does not match the product capability.'
    }
    $expected = @('libzstd.dll', 'libwim-15.dll')
    if ($HipEnabled) { $expected += 'superzip_hip_kernels.dll' }
    $entries = @($manifest.packaged_runtime_files)
    if ($entries.Count -ne $expected.Count) { throw 'Packaged runtime manifest has an unexpected entry count.' }
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        if ($entry.name -notin $expected -or -not $seen.Add([string]$entry.name) -or
            $entry.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw 'Packaged runtime manifest has an invalid or duplicate identity.'
        }
        $file = Get-Item -LiteralPath (Join-Path $PackageRoot "bin/$($entry.name)") -Force
        if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Packaged runtime is redirected or not a file: $($entry.name)"
        }
        $runtimeStream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try { $actual = (Get-FileHash -InputStream $runtimeStream -Algorithm SHA256).Hash.ToLowerInvariant() }
        finally { $runtimeStream.Dispose() }
        if ($actual -cne $entry.sha256) { throw "Packaged DLL bytes differ from the build identity: $($entry.name)" }
    }
}

# Purpose: Admit truthful capability diagnostics without pretending a hardware-free host executed GPU work.
# Inputs: Output/ExitCode came from the installed CLI; payload bytes must already have passed integrity validation.
# Outputs: Accepts ready, missing-driver or missing-device states; rejects damaged modules and contradictory output.
function Assert-SuperZipHipDependencyState {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Output, [int]$ExitCode)

    $flags = @{}
    foreach ($name in @('hip_compiled', 'available', 'hip_runtime_loadable', 'hip_kernel_loadable')) {
        $lines = @($Output | Where-Object { $_ -cmatch "^${name}=" })
        if ($lines.Count -ne 1 -or $lines[0] -cnotmatch "^${name}=(true|false)$") {
            throw "Release capability flag is missing, duplicated or invalid: $name"
        }
        $flags[$name] = $lines[0] -ceq "${name}=true"
    }
    if (-not $flags.hip_compiled) { throw 'Release artifact is not HIP-enabled.' }
    $ready = $flags.available
    $runtime = $flags.hip_runtime_loadable
    $kernel = $flags.hip_kernel_loadable
    $valid = switch ($ExitCode) {
        0 { $ready -and $runtime -and $kernel }
        11 { -not $ready -and -not $runtime -and -not $kernel }
        12 { -not $ready -and $runtime -and -not $kernel }
        default { $false }
    }
    if (-not $valid) { throw "Release dependency check failed or contradicted its capability state: $ExitCode" }
}
