param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$EnableHip,
    [switch]$CpuOnlyValidation,
    [switch]$ConfigureOnly,
    [string]$HipArch = "gfx1201",
    [string]$VcvarsVersion = "",
    [string]$PackageVersion = "",
    [string]$MsiProductIdentity = "",
    [ValidateSet("perUser", "perMachine")] [string]$MsiInstallScope = "perMachine"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
. (Join-Path $PSScriptRoot "version.ps1")

# Purpose: Find a usable CMake executable on a Windows development or CI host.
# Inputs: None; probes known install paths and PATH.
# Outputs: Returns the CMake executable path or throws when CMake is unavailable.
function Find-CMake {
    $candidates = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "CMake was not found."
}

# Purpose: Invoke a native executable and promote non-zero process exits to PowerShell failures.
# Inputs: FilePath is the executable; Arguments is the argv array; Operation is the diagnostic label.
# Outputs: Returns no value; throws when the native executable reports failure.
function Invoke-NativeTool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$Operation
    )

    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Operation failed with exit code $exitCode."
    }
}

# Purpose: Fail early when a running SuperZip instance locks the build output executable.
# Inputs: `Configuration` selects the build subdirectory that would be overwritten.
# Outputs: Throws with an actionable message when the target GUI binary is running.
function Assert-BuildOutputNotRunning {
    param([Parameter(Mandatory = $true)][string]$Configuration)

    $targetExe = [IO.Path]::GetFullPath((Join-Path $repo "build\$Configuration\SuperZip.exe"))
    foreach ($process in @(Get-Process -Name "SuperZip" -ErrorAction SilentlyContinue)) {
        $processPath = ""
        try {
            $processPath = [string]$process.Path
        } catch {
            $processPath = ""
        }
        if ([string]::IsNullOrWhiteSpace($processPath)) {
            continue
        }
        $fullProcessPath = [IO.Path]::GetFullPath($processPath)
        if ($fullProcessPath.Equals($targetExe, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Close the running build output before rebuilding: $targetExe"
        }
    }
}

# Purpose: Create a bounded MSI ProductCode identity for local build outputs.
# Inputs: None; reads the current git commit when available and appends a UTC timestamp.
# Outputs: Returns a single-line token safe for CMake and Windows Installer identity derivation.
function Get-MsiProductIdentity {
    $commit = "nogit"
    $gitOutput = & git -C $repo rev-parse --short=12 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $gitOutput) {
        $commit = [string]$gitOutput
    }
    return "local-$commit-$((Get-Date).ToUniversalTime().ToString("yyyyMMddHHmmss"))"
}

# Purpose: Validate a caller-provided MSI product identity before passing it to CMake.
# Inputs: Identity is a package/build token used in ProductCode derivation.
# Outputs: Returns the trimmed identity or throws when it is empty or unsafe.
function Assert-MsiProductIdentity {
    param([Parameter(Mandatory = $true)][string]$Identity)

    $trimmed = $Identity.Trim()
    if (-not $trimmed) {
        throw "MSI product identity must not be empty."
    }
    if ($trimmed -notmatch '^[A-Za-z0-9_.:-]+$') {
        throw "MSI product identity may contain only letters, digits, underscore, dot, colon, or hyphen."
    }
    return $trimmed
}

$cmake = Find-CMake
if ($EnableHip.IsPresent -and $CpuOnlyValidation.IsPresent) {
    throw "-EnableHip and -CpuOnlyValidation are mutually exclusive."
}
$hipArg = if ($CpuOnlyValidation.IsPresent) { "OFF" } else { "ON" }
$PackageVersion = Resolve-SuperZipPackageVersion -RepoRoot $repo -RequestedVersion $PackageVersion
if (-not $MsiProductIdentity.Trim()) {
    $MsiProductIdentity = Get-MsiProductIdentity
}
$MsiProductIdentity = Assert-MsiProductIdentity -Identity $MsiProductIdentity
$configureArgs = @(
    "-S", $repo,
    "-B", $build,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DSUPERZIP_ENABLE_HIP=$hipArg",
    "-DSUPERZIP_HIP_ARCH=$HipArch",
    "-DSUPERZIP_VCVARS_VERSION=$VcvarsVersion",
    "-DSUPERZIP_PACKAGE_VERSION=$PackageVersion",
    "-DSUPERZIP_MSI_INSTALL_SCOPE=$MsiInstallScope",
    "-DSUPERZIP_MSI_PRODUCT_IDENTITY=$MsiProductIdentity",
    "-DSUPERZIP_BUILD_GUI=ON",
    "-DSUPERZIP_BUILD_TESTS=ON"
)
Invoke-NativeTool -FilePath $cmake -Arguments $configureArgs -Operation "CMake configure"

if (-not $ConfigureOnly) {
    Assert-BuildOutputNotRunning -Configuration $Configuration
    $buildArgs = @("--build", $build, "--config", $Configuration, "--parallel")
    Invoke-NativeTool -FilePath $cmake -Arguments $buildArgs -Operation "CMake build"
}
