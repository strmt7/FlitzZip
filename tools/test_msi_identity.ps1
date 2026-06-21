param()

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
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
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "CMake was not found."
}

# Purpose: Invoke CMake and treat any non-zero exit as a test failure.
# Inputs: CMakePath is the resolved cmake.exe path; Arguments is the argv list; Label names the operation.
# Outputs: Returns no value; throws when CMake fails.
function Invoke-CMakeChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CMakePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $output = & $CMakePath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | Write-Output
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

# Purpose: Increment the numeric patch component for installer identity drift tests.
# Inputs: PackageVersion is a SuperZip SemVer string without build metadata.
# Outputs: Returns a SemVer string with the same major/minor and patch + 1.
function Get-NextPatchPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-SuperZipPackageVersion -Version $PackageVersion
    $numeric = Get-SuperZipNumericPackageVersion -PackageVersion $normalized
    $parts = $numeric.Split(".")
    return "$($parts[0]).$($parts[1]).$([int]$parts[2] + 1)"
}

# Purpose: Remove only this test's temporary configure directory.
# Inputs: Path is the candidate directory to remove.
# Outputs: Deletes the directory or throws when the path is outside the expected temp root.
function Remove-MsiIdentityTempDirectory {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "Low")]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $tempRoot = [IO.Path]::GetFullPath((Join-Path $repo "out\msi-identity-"))
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove unexpected path $resolved."
    }
    if ($PSCmdlet.ShouldProcess($resolved, "Remove MSI identity temporary configure directory")) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

# Purpose: Configure a lightweight CPack tree and read its MSI identity fields.
# Inputs: CMakePath is cmake.exe; Name identifies the temp directory; PackageVersion and Scope select MSI identity.
# Outputs: Returns ProductCode, UpgradeCode, ProductVersion, and build path metadata.
function Get-MsiIdentitySnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CMakePath,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion,
        [ValidateSet("perUser", "perMachine")]
        [string]$Scope = "perMachine"
    )

    $buildRoot = Join-Path $repo "out\msi-identity-$Name"
    Remove-MsiIdentityTempDirectory -Path $buildRoot
    $configureArgs = @(
        "-S", $repo,
        "-B", $buildRoot,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DSUPERZIP_ENABLE_HIP=OFF",
        "-DSUPERZIP_BUILD_GUI=OFF",
        "-DSUPERZIP_BUILD_TESTS=OFF",
        "-DSUPERZIP_PACKAGE_VERSION=$PackageVersion",
        "-DSUPERZIP_MSI_INSTALL_SCOPE=$Scope"
    )
    Invoke-CMakeChecked -CMakePath $CMakePath -Arguments $configureArgs -Label "CMake configure for $Name"

    return [pscustomobject][ordered]@{
        name = $Name
        packageVersion = $PackageVersion
        scope = $Scope
        productVersion = Read-SuperZipCPackValue -BuildRoot $buildRoot -Name "CPACK_PACKAGE_VERSION"
        productCode = Read-SuperZipCPackValue -BuildRoot $buildRoot -Name "CPACK_WIX_PRODUCT_GUID"
        upgradeCode = Read-SuperZipCPackValue -BuildRoot $buildRoot -Name "CPACK_WIX_UPGRADE_GUID"
        buildRoot = $buildRoot
    }
}

$cmake = Find-CMake
$baseVersion = Resolve-SuperZipProjectVersion -RepoRoot $repo
$nextPatchVersion = Get-NextPatchPackageVersion -PackageVersion $baseVersion
$snapshots = @()

try {
    $snapshots += Get-MsiIdentitySnapshot -CMakePath $cmake -Name "same-a" -PackageVersion $baseVersion -Scope "perMachine"
    $snapshots += Get-MsiIdentitySnapshot -CMakePath $cmake -Name "same-b" -PackageVersion $baseVersion -Scope "perMachine"
    $snapshots += Get-MsiIdentitySnapshot -CMakePath $cmake -Name "next-patch" -PackageVersion $nextPatchVersion -Scope "perMachine"
    $snapshots += Get-MsiIdentitySnapshot -CMakePath $cmake -Name "same-per-user" -PackageVersion $baseVersion -Scope "perUser"

    if ($snapshots[0].productCode -ne $snapshots[1].productCode) {
        throw "Same version and install scope produced different MSI ProductCode values."
    }
    if ($snapshots[0].productCode -eq $snapshots[2].productCode) {
        throw "Patch-version update did not change the MSI ProductCode."
    }
    if ($snapshots[0].productCode -eq $snapshots[3].productCode) {
        throw "Per-machine and per-user MSI scopes produced the same ProductCode."
    }
    foreach ($snapshot in $snapshots) {
        if ($snapshot.upgradeCode -ne $snapshots[0].upgradeCode) {
            throw "MSI UpgradeCode changed for snapshot $($snapshot.name)."
        }
        Write-Output ("msi_identity name={0} version={1} scope={2} product_version={3} product_code={4} upgrade_code={5}" -f
            $snapshot.name,
            $snapshot.packageVersion,
            $snapshot.scope,
            $snapshot.productVersion,
            $snapshot.productCode,
            $snapshot.upgradeCode)
    }
} finally {
    foreach ($snapshot in $snapshots) {
        if ($snapshot -and $snapshot.PSObject.Properties["buildRoot"]) {
            Remove-MsiIdentityTempDirectory -Path $snapshot.buildRoot
        }
    }
}

Write-Output "MSI identity smoke passed."
