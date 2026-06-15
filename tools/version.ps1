$script:SuperZipSemVerPattern = '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$'

# Purpose: Validate and normalize a SuperZip package version.
# Inputs: Version is a SemVer string without a `v` prefix or build metadata.
# Outputs: Returns the trimmed version or throws when the value is unsafe for release artifacts.
function Assert-SuperZipPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $normalized = $Version.Trim()
    if (-not $normalized) {
        throw "Package version must not be empty."
    }
    if ($normalized -ieq "latest" -or $normalized.StartsWith("v", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Package version must not be latest and must not use a v prefix."
    }
    if ($normalized -notmatch $script:SuperZipSemVerPattern) {
        throw "Package version must be SemVer without build metadata, for example 0.1.0 or 0.1.0-beta.1."
    }
    return $normalized
}

# Purpose: Read the default product version from the CMake project declaration.
# Inputs: RepoRoot is the repository root containing CMakeLists.txt.
# Outputs: Returns the validated project version string.
function Resolve-SuperZipProjectVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $cmakeLists = Join-Path $RepoRoot "CMakeLists.txt"
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $match = [regex]::Match($text, "project\s*\(\s*SuperZip\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", "IgnoreCase")
    if (-not $match.Success) {
        throw "Unable to resolve package version from CMakeLists.txt."
    }
    return Assert-SuperZipPackageVersion -Version $match.Groups[1].Value
}

# Purpose: Resolve the package version from an explicit argument or CMake project metadata.
# Inputs: RepoRoot is the repository root; RequestedVersion is an optional SemVer override.
# Outputs: Returns the validated version used for build diagnostics and package filenames.
function Resolve-SuperZipPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [string]$RequestedVersion = ""
    )

    if ($RequestedVersion.Trim()) {
        return Assert-SuperZipPackageVersion -Version $RequestedVersion
    }
    return Resolve-SuperZipProjectVersion -RepoRoot $RepoRoot
}

# Purpose: Read a value from CMakeCache.txt.
# Inputs: BuildRoot points at the configured build directory; Name is the cache variable.
# Outputs: Returns the cache value or throws when the variable is absent.
function Read-SuperZipCMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $cachePath = Join-Path $BuildRoot "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        throw "CMake cache not found. Run tools/build.ps1 first."
    }
    $pattern = "^$([regex]::Escape($Name)):[^=]*=(.*)$"
    $line = Get-Content -LiteralPath $cachePath |
        Where-Object { $_ -match $pattern } |
        Select-Object -First 1
    if (-not $line) {
        throw "CMake cache variable $Name was not found."
    }
    return ([regex]::Match($line, $pattern)).Groups[1].Value
}

# Purpose: Read a string assignment from CPackConfig.cmake.
# Inputs: BuildRoot points at the configured build directory; Name is the CPack variable.
# Outputs: Returns the CPack value or throws when the variable is absent.
function Read-SuperZipCPackValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $configPath = Join-Path $BuildRoot "CPackConfig.cmake"
    if (-not (Test-Path -LiteralPath $configPath)) {
        throw "CPack config not found. Run tools/build.ps1 first."
    }
    $pattern = "^set\($([regex]::Escape($Name)) `"([^`"]*)`"\)$"
    $line = Get-Content -LiteralPath $configPath |
        Where-Object { $_ -match $pattern } |
        Select-Object -First 1
    if (-not $line) {
        throw "CPack variable $Name was not found."
    }
    return ([regex]::Match($line, $pattern)).Groups[1].Value
}

# Purpose: Derive Windows Installer-compatible numeric version metadata from SemVer.
# Inputs: PackageVersion is a validated SemVer package version.
# Outputs: Returns the major.minor.patch numeric version string.
function Get-SuperZipNumericPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-SuperZipPackageVersion -Version $PackageVersion
    $match = [regex]::Match($normalized, $script:SuperZipSemVerPattern)
    return "$($match.Groups[1].Value).$($match.Groups[2].Value).$($match.Groups[3].Value)"
}

# Purpose: Build the package artifact basename from a validated product version.
# Inputs: PackageVersion is a SemVer package version.
# Outputs: Returns the artifact basename without extension.
function Get-SuperZipPackageBase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-SuperZipPackageVersion -Version $PackageVersion
    return "SuperZip-$normalized-win64"
}

# Purpose: Ensure package script inputs match the configured build tree before artifacts are created.
# Inputs: BuildRoot is the configured build directory; PackageVersion is the requested package version.
# Outputs: Returns no value; throws when cache or CPack metadata would produce mislabeled artifacts.
function Assert-SuperZipPackageVersionMatchesBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-SuperZipPackageVersion -Version $PackageVersion
    $expectedPackageBase = Get-SuperZipPackageBase -PackageVersion $normalized
    $expectedNumericVersion = Get-SuperZipNumericPackageVersion -PackageVersion $normalized
    $buildVersion = Read-SuperZipCMakeCacheValue -BuildRoot $BuildRoot -Name "SUPERZIP_PACKAGE_VERSION"
    if ($buildVersion -ne $normalized) {
        throw "Requested package version $normalized does not match configured build version $buildVersion. Re-run tools/build.ps1 with -PackageVersion $normalized before packaging."
    }
    $cpackFileName = Read-SuperZipCPackValue -BuildRoot $BuildRoot -Name "CPACK_PACKAGE_FILE_NAME"
    if ($cpackFileName -ne $expectedPackageBase) {
        throw "CPack package file name $cpackFileName does not match expected $expectedPackageBase."
    }
    $cpackVersion = Read-SuperZipCPackValue -BuildRoot $BuildRoot -Name "CPACK_PACKAGE_VERSION"
    if ($cpackVersion -ne $expectedNumericVersion) {
        throw "CPack package version $cpackVersion does not match expected numeric version $expectedNumericVersion."
    }
}
