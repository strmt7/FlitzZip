param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [string]$DependencyFile = "",
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,
    [string]$Arch = "gfx1201",
    [string]$VcvarsVersion = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "hip_architecture.ps1")
$Arch = Resolve-HipArchitecture -Architecture $Arch

# Purpose: Reject values that cannot be safely embedded in the generated cmd.exe command line.
# Inputs: Name is the diagnostic label; Value is the string to validate.
# Outputs: Throws on invalid multiline values and otherwise returns no value.
function Assert-SingleLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [AllowEmptyString()]
        [string]$Value
    )
    if ($Value -match "[`r`n]") {
        throw "$Name must be a single-line value."
    }
}

# Purpose: Find Visual Studio's vcvarsall.bat across hosted CI and local installations.
# Inputs: None; probes VSINSTALLDIR, vswhere, and known Visual Studio edition paths.
# Outputs: Returns the absolute path to vcvarsall.bat or throws when the C++ toolset is unavailable.
function Find-VcvarsAll {
    $roots = New-Object System.Collections.Generic.List[string]
    if ($env:VSINSTALLDIR) {
        $roots.Add($env:VSINSTALLDIR.TrimEnd("\"))
    }

    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $vswhereCandidates = @()
    if ($programFilesX86) {
        $vswhereCandidates += (Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe")
    }
    $vswhereCandidates += "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

    $vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($vswhere) {
        $detectedRoots = & $vswhere -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        foreach ($root in $detectedRoots) {
            if ($root) {
                $roots.Add($root.TrimEnd("\"))
            }
        }
    }

    foreach ($majorVersion in @("18", "2022")) {
        foreach ($edition in @("Enterprise", "Professional", "Community", "BuildTools")) {
            $roots.Add("C:\Program Files\Microsoft Visual Studio\$majorVersion\$edition")
        }
    }

    $seen = @{}
    foreach ($root in $roots) {
        if (-not $root) {
            continue
        }
        $key = $root.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            continue
        }
        $seen[$key] = $true
        $candidate = Join-Path $root "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "vcvarsall.bat was not found. Install Visual Studio with the Microsoft.VisualStudio.Component.VC.Tools.x86.x64 component."
}

# Purpose: Enumerate MSVC toolsets available under the Visual Studio instance that owns vcvarsall.bat.
# Inputs: VcvarsAll is the resolved path to Visual Studio's vcvarsall.bat.
# Outputs: Returns installed MSVC toolset directory names sorted from newest to oldest.
function Get-MsvcToolsetVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$VcvarsAll
    )

    $buildDir = Split-Path -Parent $VcvarsAll
    $auxiliaryDir = Split-Path -Parent $buildDir
    $vcDir = Split-Path -Parent $auxiliaryDir
    $toolsetRoot = Join-Path $vcDir "Tools\MSVC"
    if (-not (Test-Path -LiteralPath $toolsetRoot)) {
        return @()
    }

    return @(
        Get-ChildItem -LiteralPath $toolsetRoot -Directory |
            Where-Object { $_.Name -match '^[0-9]+(\.[0-9]+){1,3}$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { $_.Name }
    )
}

# Purpose: Choose a HIP-compatible MSVC toolset prefix when the caller did not provide one.
# Inputs: AvailableVersions is the installed MSVC list; RequestedVersion is the optional caller override.
# Outputs: Returns candidate vcvars versions in preferred order, with an empty string meaning Visual Studio default.
function Resolve-VcvarsVersionCandidate {
    param(
        [string[]]$AvailableVersions,
        [AllowEmptyString()]
        [string]$RequestedVersion
    )

    if ($RequestedVersion) {
        return @($RequestedVersion)
    }

    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($prefix in @("14.44", "14.42")) {
        $match = @(
            $AvailableVersions |
                Where-Object { $_ -eq $prefix -or $_.StartsWith("$prefix.") } |
                Sort-Object { [version]$_ } -Descending |
                Select-Object -First 1
        )
        if ($match.Count -gt 0) {
            $candidates.Add($prefix)
            break
        }
    }

    $candidates.Add("")
    return @($candidates.ToArray())
}

# Purpose: Escape one compiler-reported path for a CMake Make-format dependency rule.
# Inputs: Path is a source/header/output path; Windows separators are normalized before escaping.
# Outputs: Returns a pathname with literal spaces, hashes and dollar signs preserved.
function ConvertTo-HipDependencyPath {
    param([string]$Path)
    return $Path.Replace('\', '/').Replace('$', '$$').Replace('#', '\#').Replace(' ', '\ ')
}

# Purpose: Complete host dependencies with actual device-preprocessor inputs for every requested architecture.
# Inputs: Prefix selects MSVC/compiler; CompilerArguments contains the compilation's shared language/define/include flags.
# Outputs: Appends deduplicated device dependency rules, streams away preprocessed code, and returns native status.
function Add-HipDeviceDependency {
    param([string]$Prefix, [string]$CompilerArguments)
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($architecture in $Arch.Split(',')) {
        $scan = "$Prefix --offload-arch=$architecture --offload-device-only $CompilerArguments -E `"$Source`""
        cmd /c $scan | ForEach-Object {
            if ([string]$_ -match '^#\s+\d+\s+"((?:[^"\\]|\\.)*)"') {
                $path = $Matches[1].Replace('\\', '\')
                if (-not $path.StartsWith('<')) {
                    [void]$paths.Add([IO.Path]::GetFullPath($path))
                }
            }
        }
        if ($LASTEXITCODE -ne 0) { return $LASTEXITCODE }
    }
    if ($paths.Count -eq 0) { throw 'HIP device preprocessor returned no dependency paths.' }
    $escaped = @($paths | Sort-Object | ForEach-Object { ConvertTo-HipDependencyPath -Path $_ })
    $rule = (ConvertTo-HipDependencyPath -Path $Output) + ': ' + ($escaped -join ' ') + "`n"
    [IO.File]::AppendAllText($DependencyFile, "`n" + $rule, [Text.UTF8Encoding]::new($false))
    return 0
}

# Purpose: Discard only the current compiler attempt's object and optional dependency data.
# Inputs: Output and DependencyFile are the validated, caller-selected artifact paths; no recursive deletion occurs.
# Outputs: Removes stale or failed artifacts, or throws on a filesystem failure.
function Invoke-HipCompileCleanup {
    foreach ($path in @($Output, $DependencyFile)) {
        if ($path -and (Test-Path -LiteralPath $path)) { Remove-Item -LiteralPath $path -Force }
    }
}

# Purpose: Build the HIP object with one Visual Studio environment candidate.
# Inputs: CandidateVersion selects MSVC; validated Arch selects GPU images. Optional DependencyFile receives compiler-discovered inputs.
# Outputs: Returns hipcc's native exit code; rejects successful compilation without a requested dependency file.
function Invoke-HipCompile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$VcvarsAll,
        [AllowEmptyString()]
        [string]$CandidateVersion,
        [Parameter(Mandatory = $true)]
        [string]$HipccPath,
        [Parameter(Mandatory = $true)]
        [string]$IncludePath
    )

    $vcvarsArgs = "amd64"
    if ($CandidateVersion) {
        $vcvarsArgs += " -vcvars_ver=$CandidateVersion"
        Write-Information "Compiling HIP object with MSVC toolset $CandidateVersion." -InformationAction Continue
    } else {
        Write-Information "Compiling HIP object with the Visual Studio default MSVC toolset." -InformationAction Continue
    }

    Invoke-HipCompileCleanup
    $dependencyArguments = ""
    if ($DependencyFile) {
        $dependencyArguments = "-MD -MF `"$DependencyFile`" -MQ `"$Output`""
    }

    $offloadArguments = Get-HipOffloadArgument -Architecture $Arch
    $prefix = "call `"$VcvarsAll`" $vcvarsArgs >nul && `"$HipccPath`""
    $compilerArguments = "-std=c++20 -O3 -fms-runtime-lib=static -DSUPERZIP_ENABLE_HIP=1 -I`"$IncludePath`""
    $cmd = "$prefix $offloadArguments $dependencyArguments $compilerArguments -c `"$Source`" -o `"$Output`""
    try {
        cmd /c $cmd | ForEach-Object { Write-Information ([string]$_) -InformationAction Continue }
    } catch {
        Invoke-HipCompileCleanup
        throw
    }
    $compilerExitCode = $LASTEXITCODE
    if ($compilerExitCode -eq 0 -and $DependencyFile -and
        (-not (Test-Path -LiteralPath $DependencyFile -PathType Leaf) -or
            (Get-Item -LiteralPath $DependencyFile).Length -eq 0)) {
        Invoke-HipCompileCleanup
        throw "HIP compiler succeeded without a nonempty requested dependency file."
    }
    if ($compilerExitCode -eq 0 -and $DependencyFile) {
        try {
            $compilerExitCode = Add-HipDeviceDependency -Prefix $prefix -CompilerArguments $compilerArguments
        } catch {
            Invoke-HipCompileCleanup
            throw
        }
    }
    if ($compilerExitCode -ne 0) {
        Invoke-HipCompileCleanup
    }
    return $compilerExitCode
}

if (-not $env:HIP_PATH) {
    throw "HIP_PATH is not set."
}

$hipcc = Join-Path $env:HIP_PATH "bin\hipcc.exe"
if (-not (Test-Path $hipcc)) {
    throw "hipcc.exe not found at $hipcc"
}

if ($VcvarsVersion -and $VcvarsVersion -notmatch '^[0-9]+(\.[0-9]+)*$') {
    throw "VcvarsVersion must be empty or a dotted MSVC toolset version such as 14.44."
}

Assert-SingleLine -Name "Source" -Value $Source
Assert-SingleLine -Name "Output" -Value $Output
Assert-SingleLine -Name "DependencyFile" -Value $DependencyFile
Assert-SingleLine -Name "RepoRoot" -Value $RepoRoot
Assert-SingleLine -Name "Arch" -Value $Arch
Assert-SingleLine -Name "VcvarsVersion" -Value $VcvarsVersion

$vcvars = Find-VcvarsAll

$outputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$include = Join-Path $RepoRoot "src"
$availableToolsets = @(Get-MsvcToolsetVersion -VcvarsAll $vcvars)
$vcvarsCandidates = @(Resolve-VcvarsVersionCandidate -AvailableVersions $availableToolsets -RequestedVersion $VcvarsVersion)

$attempts = New-Object System.Collections.Generic.List[string]
foreach ($candidate in $vcvarsCandidates) {
    $label = if ($candidate) { $candidate } else { "default" }
    $exitCode = Invoke-HipCompile -VcvarsAll $vcvars -CandidateVersion $candidate -HipccPath $hipcc -IncludePath $include
    if ($exitCode -eq 0) {
        return
    }
    $attempts.Add("${label}: exit code $exitCode")
}

throw "hipcc failed for all MSVC toolset candidates ($($attempts -join '; '))."
