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
$vcvarsArgs = "amd64"
if ($VcvarsVersion) {
    $vcvarsArgs += " -vcvars_ver=$VcvarsVersion"
}
$cmd = "call `"$vcvars`" $vcvarsArgs >nul && `"$hipcc`" --offload-arch=$Arch -std=c++20 -O3 -fms-runtime-lib=static -DSUPERZIP_ENABLE_HIP=1 -I`"$include`" -c `"$Source`" -o `"$Output`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "hipcc failed with exit code $LASTEXITCODE"
}
