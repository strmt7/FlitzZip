# Purpose: Validate installed runtime bytes independently from optional GPU hardware availability.
# Inputs: PackageRoot is an installed/staged payload; HipEnabled is its configured product capability.
# Outputs: Throws for missing, redirected, duplicate, unexpected or checksum-mismatched packaged runtimes.
function Assert-FlitzZipPackagedRuntime {
    param([Parameter(Mandatory = $true)][string]$PackageRoot, [bool]$HipEnabled)

    foreach ($directory in @($PackageRoot, (Join-Path $PackageRoot 'bin'))) {
        $item = Get-Item -LiteralPath $directory -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Packaged runtime directory is redirected or invalid.'
        }
    }
    $manifestPath = Join-Path $PackageRoot 'flitzzip-runtime-dependencies.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.product -ne 'FlitzZip' -or $manifest.gpu_backend.enabled -ne $HipEnabled) {
        throw 'Packaged runtime manifest does not match the product capability.'
    }
    $expected = @('libzstd.dll', 'libwim-15.dll')
    if ($HipEnabled) { $expected += 'flitzzip_hip_kernels.dll' }
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

# Run one native packaging command and reject failure before inspecting outputs.
# The executable is selected by the pinned tool resolver; arguments remain an array.
function Invoke-FlitzZipPackageTool {
    param([string]$FilePath, [string[]]$Arguments, [string]$Operation)
    $ErrorActionPreference = 'Stop'
    if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $PSNativeCommandUseErrorActionPreference = $false
    }
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "${Operation} failed with exit code $LASTEXITCODE." }
}

# Generate in a fresh owned directory, validate there, then atomically publish.
# Generate receives the new candidate path and must throw on tool failure.
# Validate receives a complete regular nonempty file; neither callback may publish it.
# Failed generation/validation preserves any preceding artifact at the destination.
function Publish-FlitzZipGeneratedPackage {
    param(
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][scriptblock]$Generate,
        [scriptblock]$Validate
    )
    $ErrorActionPreference = 'Stop'
    if ($FileName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,159}$') {
        throw 'Package artifact must have a simple filename.'
    }
    $root = Get-Item -LiteralPath $OutputDirectory -Force
    if (-not $root.PSIsContainer -or ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Package output directory is redirected or invalid.'
    }
    $destination = Join-Path $root.FullName $FileName
    $workspace = Join-Path $root.FullName ('package-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $workspace -ErrorAction Stop | Out-Null
    try {
        $candidate = Join-Path $workspace $FileName
        & $Generate $candidate | Out-Host
        $file = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -eq 0 -or
            ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Package generator did not produce a complete regular artifact.'
        }
        if ($Validate) { & $Validate $candidate | Out-Host }
        if (Test-Path -LiteralPath $destination) {
            $previous = Get-Item -LiteralPath $destination -Force
            if ($previous.PSIsContainer -or ($previous.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Package publication destination is redirected or invalid.'
            }
            [IO.File]::Replace($candidate, $destination, [NullString]::Value)
        } else {
            [IO.File]::Move($candidate, $destination)
        }
        return $destination
    } finally {
        $owned = Get-Item -LiteralPath $workspace -Force -ErrorAction Stop
        $resolved = [IO.Path]::GetFullPath($owned.FullName)
        if ($resolved -cne [IO.Path]::GetFullPath($workspace) -or
            [IO.Path]::GetDirectoryName($resolved) -cne $root.FullName -or
            ($owned.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing redirected package workspace cleanup.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
    }
}

# Purpose: Admit truthful capability diagnostics without pretending a hardware-free host executed GPU work.
# Inputs: Output/ExitCode came from the installed CLI; payload bytes must already have passed integrity validation.
# Outputs: Accepts ready, missing-driver or missing-device states; rejects damaged modules and contradictory output.
function Assert-FlitzZipHipDependencyState {
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

# Purpose: Share one pinned WiX CLI/extension identity across local and hosted consumers.
# Inputs: RepoRoot is the checkout owning the generated cache. Outputs: Versioned paths, never PATH discovery.
function Get-FlitzZipWixToolchain {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $version = '7.0.1'
    $root = Join-Path ([IO.Path]::GetFullPath($RepoRoot)) "out/tools/wix-$version"
    return [pscustomobject]@{
        Version = $version
        CliVersion = '7.0.1+642f1a0'
        Path = (Join-Path $root 'wix.exe')
        ExtensionHome = (Join-Path $root 'extension-cache')
        UiExtension = "WixToolset.UI.wixext/$version"
        UiVersion = '7.0.1+642f1a0f7c857ffa4fc9fa3f9d1433aab895dd29'
    }
}

# Purpose: Reject prefix matches, prereleases, extra output and failed native version probes.
# Inputs: Output and ExitCode come from the selected CLI; Expected is the admitted publisher identity.
# Outputs: Throws unless precisely one successful version line matches.
function Assert-FlitzZipWixVersion {
    param([AllowEmptyCollection()][string[]]$Output, [int]$ExitCode, [string]$Expected)
    if ($ExitCode -ne 0 -or @($Output).Count -ne 1 -or $Output[0] -cne $Expected) {
        throw "WiX version admission failed; expected exactly '$Expected' with exit code zero."
    }
}

# Purpose: Resolve only the pinned checkout-owned tool and validate its actual process output.
# Inputs: RepoRoot owns the versioned cache. Outputs: Qualified wix.exe; never a global-tool fallback.
function Resolve-FlitzZipWix {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $tool = Get-FlitzZipWixToolchain -RepoRoot $RepoRoot
    $directory = [IO.Path]::GetFullPath($RepoRoot)
    foreach ($part in @('out', 'tools', "wix-$($tool.Version)", 'wix.exe')) {
        $directory = Join-Path $directory $part
        $item = Get-Item -LiteralPath $directory -Force -ErrorAction Stop
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'WiX tool cache is redirected.' }
    }
    if ($item.PSIsContainer) { throw 'WiX tool is not a regular executable.' }
    $output = @(& $tool.Path --version 2>&1)
    Assert-FlitzZipWixVersion -Output $output -ExitCode $LASTEXITCODE -Expected $tool.CliVersion
    return $tool.Path
}

# Purpose: Scope tool discovery and extension storage to this checkout for the whole native consumer.
# Inputs: RepoRoot owns the admitted tool; Action performs packaging. Outputs: Propagates failure and restores caller state.
function Invoke-FlitzZipWixEnvironment {
    param([Parameter(Mandatory = $true)][string]$RepoRoot, [Parameter(Mandatory = $true)][scriptblock]$Action)
    $tool = Get-FlitzZipWixToolchain -RepoRoot $RepoRoot
    $wix = Resolve-FlitzZipWix -RepoRoot $RepoRoot
    New-Item -ItemType Directory -Force -Path $tool.ExtensionHome | Out-Null
    if ((Get-Item -LiteralPath $tool.ExtensionHome -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'WiX UI cache is redirected.'
    }
    $originalPath = $env:PATH
    $originalWix = $env:WIX
    $originalExtensions = $env:WIX_EXTENSIONS
    try {
        $env:PATH = (Split-Path -Parent $wix) + [IO.Path]::PathSeparator + $originalPath
        $env:WIX = Split-Path -Parent $wix
        $env:WIX_EXTENSIONS = $tool.ExtensionHome
        & $Action $wix $tool.UiExtension
    } finally {
        $env:PATH = $originalPath
        $env:WIX = $originalWix
        $env:WIX_EXTENSIONS = $originalExtensions
    }
}

# Purpose: Bind CPack to the exact installed UI assembly, bypassing versionless cache selection.
# Inputs: RepoRoot owns the provisioned extension. Outputs: Documented CPack overrides preserving its standard UI namespace.
function Get-FlitzZipWixUiArgument {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $tool = Get-FlitzZipWixToolchain -RepoRoot $RepoRoot
    $directory = $tool.ExtensionHome
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'WiX UI cache is redirected.'
    }
    foreach ($part in @('.wix', 'extensions', 'WixToolset.UI.wixext', $tool.Version, 'wixext7', 'WixToolset.UI.wixext.dll')) {
        $directory = Join-Path $directory $part
        $item = Get-Item -LiteralPath $directory -Force -ErrorAction Stop
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'WiX UI cache is redirected.' }
    }
    if ($item.PSIsContainer -or $item.VersionInfo.ProductVersion -cne $tool.UiVersion) {
        throw 'WiX UI extension identity differs from the pinned release.'
    }
    return @('-D', "CPACK_WIX_EXTENSIONS=$directory", '-D', 'CPACK_WIX_SKIP_WIX_UI_EXTENSION=ON',
        '-D', 'CPACK_WIX_CUSTOM_XMLNS=ui=http://wixtoolset.org/schemas/v4/wxs/ui')
}
