param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,
    [string]$Arch = "gfx1201",
    [string]$VcvarsVersion = ""
)

$ErrorActionPreference = "Stop"

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
function Get-MsvcToolsetVersions {
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
function Resolve-VcvarsVersionCandidates {
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

# Purpose: Build the HIP object with one Visual Studio environment candidate.
# Inputs: CandidateVersion is a vcvars toolset prefix or empty for default Visual Studio; other values come from script parameters.
# Outputs: Returns hipcc's native process exit code.
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
        Write-Host "Compiling HIP object with MSVC toolset $CandidateVersion."
    } else {
        Write-Host "Compiling HIP object with the Visual Studio default MSVC toolset."
    }

    if (Test-Path -LiteralPath $Output) {
        Remove-Item -LiteralPath $Output -Force
    }

    $cmd = "call `"$VcvarsAll`" $vcvarsArgs >nul && `"$HipccPath`" --offload-arch=$Arch -std=c++20 -O3 -fms-runtime-lib=static -DSUPERZIP_ENABLE_HIP=1 -I`"$IncludePath`" -c `"$Source`" -o `"$Output`""
    cmd /c $cmd
    return $LASTEXITCODE
}

if (-not $env:HIP_PATH) {
    throw "HIP_PATH is not set."
}

$hipcc = Join-Path $env:HIP_PATH "bin\hipcc.exe"
if (-not (Test-Path $hipcc)) {
    throw "hipcc.exe not found at $hipcc"
}

if ($Arch -notmatch '^gfx[0-9a-z]+$') {
    throw "Arch must look like gfx1201."
}
if ($VcvarsVersion -and $VcvarsVersion -notmatch '^[0-9]+(\.[0-9]+)*$') {
    throw "VcvarsVersion must be empty or a dotted MSVC toolset version such as 14.44."
}

Assert-SingleLine -Name "Source" -Value $Source
Assert-SingleLine -Name "Output" -Value $Output
Assert-SingleLine -Name "RepoRoot" -Value $RepoRoot
Assert-SingleLine -Name "Arch" -Value $Arch
Assert-SingleLine -Name "VcvarsVersion" -Value $VcvarsVersion

$vcvars = Find-VcvarsAll

$outputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$include = Join-Path $RepoRoot "src"
$availableToolsets = @(Get-MsvcToolsetVersions -VcvarsAll $vcvars)
$vcvarsCandidates = @(Resolve-VcvarsVersionCandidates -AvailableVersions $availableToolsets -RequestedVersion $VcvarsVersion)

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
