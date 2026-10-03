param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$EnableHip,
    [switch]$CpuOnlyValidation,
    [switch]$ConfigureOnly,
    [ValidateSet("", "Visual Studio 17 2022", "Visual Studio 18 2026")]
    [string]$Generator = "",
    [string]$HipArch = "release",
    [string]$HipPath = "",
    [string]$VcvarsVersion = "",
    [string]$PackageVersion = "",
    [string]$MsiProductIdentity = "",
    [ValidateSet("perUser", "perMachine")] [string]$MsiInstallScope = "perMachine"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
. (Join-Path $PSScriptRoot "version.ps1")
. (Join-Path $PSScriptRoot "hip_architecture.ps1")
. (Join-Path $PSScriptRoot "build_parallelism.ps1")
. (Join-Path $PSScriptRoot "cmake_toolchain.ps1")
. (Join-Path $PSScriptRoot "rocm_toolchain.ps1")

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

# Purpose: Invoke the shared receipt implementation without mixing JSON and build diagnostics.
# Inputs: Operation and explicit arguments for this owned build transaction.
# Outputs: Parsed receipt summary; throws on a failed producer invocation.
function Invoke-NativeBuildReceipt {
    param([string]$Operation, [string[]]$Arguments = @())
    $output = @(& py -3 (Join-Path $PSScriptRoot 'native_build_receipt.py') $Operation --root $repo @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Native build receipt $Operation failed with exit code $LASTEXITCODE." }
    return (($output -join [Environment]::NewLine) | ConvertFrom-Json)
}

$HipArch = Resolve-HipArchitecture -Architecture $HipArch
$cmake = Find-CMake -RepoRoot $repo
$Generator = Find-CMakeGenerator -Requested $Generator -BuildRoot $build
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
    "-G", $Generator,
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
if (-not $CpuOnlyValidation) {
    $sdkRoot = Resolve-RocmSdkRoot -RepoRoot $repo -RequestedPath $HipPath
    $configureArgs += "-DSUPERZIP_HIP_PATH=$sdkRoot"
}
$null = New-Item -ItemType Directory -Force -Path $build
if ((Get-Item -LiteralPath $build).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Refusing a linked native build directory.'
}
$lockPath = Join-Path $build 'native-build.lock'
if ((Test-Path -LiteralPath $lockPath) -and
    ((Get-Item -LiteralPath $lockPath).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Refusing a linked native build lock.'
}
# One tree-wide lock also serializes configurations sharing CMakeCache.txt.
$buildLock = [IO.FileStream]::new($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$transaction = $null
$completed = $false
try {
    $transaction = Invoke-NativeBuildReceipt -Operation begin -Arguments @('--configuration', $Configuration)
    $receiptArguments = @('--token', $transaction.transaction_id, '--cmake', $cmake)
    Invoke-NativeTool -FilePath $cmake -Arguments $configureArgs -Operation "CMake configure"
    if ($ConfigureOnly) {
        Invoke-NativeBuildReceipt -Operation configure-only -Arguments @('--token', $transaction.transaction_id) | Out-Null
        $completed = $true
    } else {
        Assert-BuildOutputNotRunning -Configuration $Configuration
        $prepared = Invoke-NativeBuildReceipt -Operation prepare -Arguments $receiptArguments
        $jobs = Resolve-BuildParallelism
        $buildArgs = @("--build", $build, "--config", $Configuration, "--parallel", [string]$jobs)
        if ($prepared.clean_first_required) {
            Write-Output 'Native receipt requires a fresh build for the first receipt or changed configuration/toolchain/artifacts.'
            $buildArgs += '--clean-first'
            $receiptArguments += '--clean-first'
        }
        Invoke-SuperZipParallelBuild -Jobs $jobs -Action {
            Invoke-NativeTool -FilePath $cmake -Arguments $buildArgs -Operation "CMake build"
        }
        $receipt = Invoke-NativeBuildReceipt -Operation finish -Arguments $receiptArguments
        Write-Output "native_build_receipt status=successful inputs_sha256=$($receipt.inputs_sha256) receipt_sha256=$($receipt.receipt_sha256)"
        $completed = $true
    }
} finally {
    try {
        if ($null -ne $transaction -and -not $completed) {
            Invoke-NativeBuildReceipt -Operation failed -Arguments @('--token', $transaction.transaction_id) | Out-Null
        }
    } finally {
        $buildLock.Dispose()
    }
}
