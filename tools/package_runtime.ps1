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

# Snapshot declared absolute files under one payload root. Each digest is read
# under a write-denying handle; duplicate declarations name one owned file.
function Get-FlitzZipPayloadSnapshot {
    param([Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$FilePath)
    $ErrorActionPreference = 'Stop'
    $root = Get-Item -LiteralPath $PackageRoot -Force
    if (-not $root.PSIsContainer -or ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Installed payload root is redirected or invalid.'
    }
    $rootPath = $root.FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    $payload = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    $checkedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $FilePath) {
        if ([string]::IsNullOrWhiteSpace($line) -or -not [IO.Path]::IsPathRooted($line)) {
            throw 'Payload declaration contains an invalid file path.'
        }
        $path = [IO.Path]::GetFullPath($line)
        if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Payload declaration contains a file outside its root.'
        }
        $name = $path.Substring($prefix.Length).Replace('\', '/')
        if ($payload.ContainsKey($name)) { continue }
        $file = Get-Item -LiteralPath $path -Force
        if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Declared payload is redirected or not a file.'
        }
        for ($parent = $file.Directory; $parent; $parent = $parent.Parent) {
            if (-not $checkedDirectories.Add($parent.FullName)) { break }
            if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Declared payload directory is redirected.'
            }
            if ($parent.FullName -ieq $rootPath) { break }
        }
        $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            $length = $stream.Length
            $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant()
        } finally { $stream.Dispose() }
        $payload.Add($name, [pscustomobject]@{ path = $file.FullName; length = $length; sha256 = $digest })
    }
    if (-not $payload.get_Count()) { throw 'Payload declaration contains no files.' }
    return ,$payload
}

# Capture only CMake-declared files, not arbitrary staging contents. Retain this
# snapshot through package publication and the installed/repair consumer checks.
function Get-FlitzZipInstalledPayload {
    param([Parameter(Mandatory = $true)][string]$StageRoot,
        [Parameter(Mandatory = $true)][string]$InstallManifest)
    $ErrorActionPreference = 'Stop'
    return ,(Get-FlitzZipPayloadSnapshot -PackageRoot $StageRoot `
        -FilePath @(Get-Content -LiteralPath $InstallManifest -Encoding UTF8))
}

# Verify every installer-owned file against captured staging bytes, independently
# of executable/runtime admission. Unmanaged files are neither removed nor adopted.
function Assert-FlitzZipInstalledPayload {
    param([Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][Collections.Generic.Dictionary[string, object]]$Payload)
    $ErrorActionPreference = 'Stop'
    $root = [IO.Path]::GetFullPath($PackageRoot)
    $paths = @($Payload.get_Keys() | ForEach-Object { Join-Path $root $_ })
    $actual = Get-FlitzZipPayloadSnapshot -PackageRoot $root -FilePath $paths
    foreach ($name in $Payload.get_Keys()) {
        if (-not $actual.ContainsKey($name) -or $actual[$name].length -ne $Payload[$name].length -or
            $actual[$name].sha256 -cne $Payload[$name].sha256) {
            throw "Installed payload differs from the staged release bytes: $name"
        }
    }
}

# Write the exact installed-file snapshot, including hidden files, through the
# existing ZIP implementation. Only relative archive names enter the artifact;
# diagnostics and any other files outside the snapshot cannot become payload.
function New-FlitzZipPortablePackage {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.Generic.Dictionary[string, object]]$Payload)
    $ErrorActionPreference = 'Stop'
    if (-not $PSCmdlet.ShouldProcess($Path, 'Create portable ZIP from the installed payload')) { return }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            # CLR accessors bypass dictionary keys named Keys, Count or Values.
            foreach ($name in @($Payload.get_Keys() | Sort-Object)) {
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $Payload[$name].path,
                    $name, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}

# Validate the newly generated ZIP against the pre-check install snapshot before
# atomic publication. Extra, missing, duplicate or changed files fail the entire
# candidate; no diagnostic filename or content category receives an exclusion.
# Read byte identities directly from a completed ZIP. Installed-file qualification
# retains this snapshot instead of CMake's manifest, which CPack can overwrite.
function Get-FlitzZipPortablePayload {
    param([Parameter(Mandatory = $true)][string]$Path)
    $ErrorActionPreference = 'Stop'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $payload = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.EndsWith('/') -and $entry.Length -eq 0) { continue }
            if ($name.EndsWith('/')) { throw "ZIP contains a nonempty directory: $name" }
            if ($payload.ContainsKey($name)) { throw "ZIP contains a duplicate installed file: $name" }
            $stream = $entry.Open()
            try { $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant() }
            finally { $stream.Dispose() }
            $payload.Add($name, [pscustomobject]@{ length = $entry.Length; sha256 = $digest })
        }
        if (-not $payload.get_Count()) { throw 'ZIP declares no payload files.' }
        return ,$payload
    } finally { $archive.Dispose() }
}

# Check the complete ZIP identity against the pre-publication CMake snapshot.
function Assert-FlitzZipPortablePayload {
    param([Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.Generic.Dictionary[string, object]]$Payload)
    $actual = Get-FlitzZipPortablePayload -Path $Path
    foreach ($name in $actual.get_Keys()) {
        if (-not $Payload.ContainsKey($name)) { throw "ZIP contains a file outside the install manifest: $name" }
        if ($actual[$name].length -ne $Payload[$name].length) { throw "ZIP installed file length differs: $name" }
        if ($actual[$name].sha256 -cne $Payload[$name].sha256) { throw "ZIP installed file bytes differ: $name" }
    }
    if ($actual.get_Count() -ne $Payload.get_Count()) { throw 'ZIP omits files from the install manifest.' }
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

# Exercise the actual portable archive in a fresh extraction, never the builder's
# staging directory. Test receives package/workspace paths and owns no files after
# returning. Its output or failure is preserved; extraction is removed in either case.
function Invoke-FlitzZipPortableSmoke {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$WorkRoot,
        [Parameter(Mandatory = $true)][scriptblock]$Test
    )
    $ErrorActionPreference = 'Stop'
    $root = Get-Item -LiteralPath $WorkRoot -Force -ErrorAction Stop
    if (-not $root.PSIsContainer -or ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Portable smoke parent is redirected or invalid.'
    }
    if (-not $PSCmdlet.ShouldProcess($ArchivePath, 'Extract and test the portable package')) { return }
    $workspace = Join-Path $root.FullName ('portable-smoke-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $workspace -ErrorAction Stop | Out-Null
    try {
        $packageRoot = Join-Path $workspace 'package'
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $packageRoot -ErrorAction Stop
        & $Test $packageRoot $workspace
    } finally {
        $owned = Get-Item -LiteralPath $workspace -Force -ErrorAction Stop
        $resolved = [IO.Path]::GetFullPath($owned.FullName)
        if ($resolved -cne [IO.Path]::GetFullPath($workspace) -or
            [IO.Path]::GetDirectoryName($resolved) -cne [IO.Path]::GetDirectoryName($workspace) -or
            ($owned.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing redirected portable smoke workspace cleanup.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
    }
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
