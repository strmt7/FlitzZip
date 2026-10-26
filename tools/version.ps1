$script:FlitzZipSemVerPattern = '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$'

# Purpose: Validate and normalize a FlitzZip package version.
# Inputs: Version is a SemVer string without a `v` prefix or build metadata.
# Outputs: Returns the trimmed version or throws when the value is unsafe for release artifacts.
function Assert-FlitzZipPackageVersion {
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
    if ($normalized -notmatch $script:FlitzZipSemVerPattern) {
        throw "Package version must be SemVer without build metadata, for example 0.1.0 or 0.1.0-beta.1."
    }
    return $normalized
}

# Purpose: Read the default product version from the CMake project declaration.
# Inputs: RepoRoot is the repository root containing CMakeLists.txt.
# Outputs: Returns the validated project version string.
function Resolve-FlitzZipProjectVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $cmakeLists = Join-Path $RepoRoot "CMakeLists.txt"
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $match = [regex]::Match($text, "project\s*\(\s*FlitzZip\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", "IgnoreCase")
    if (-not $match.Success) {
        throw "Unable to resolve package version from CMakeLists.txt."
    }
    return Assert-FlitzZipPackageVersion -Version $match.Groups[1].Value
}

# Purpose: Resolve the package version from an explicit argument or CMake project metadata.
# Inputs: RepoRoot is the repository root; RequestedVersion is an optional SemVer override.
# Outputs: Returns the validated version used for build diagnostics and package filenames.
function Resolve-FlitzZipPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [string]$RequestedVersion = ""
    )

    if ($RequestedVersion.Trim()) {
        return Assert-FlitzZipPackageVersion -Version $RequestedVersion
    }
    return Resolve-FlitzZipProjectVersion -RepoRoot $RepoRoot
}

# Purpose: Read a value from CMakeCache.txt.
# Inputs: BuildRoot points at the configured build directory; Name is the cache variable.
# Outputs: Returns the cache value or throws when the variable is absent.
function Read-FlitzZipCMakeCacheValue {
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
function Read-FlitzZipCPackValue {
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
function Get-FlitzZipNumericPackageVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-FlitzZipPackageVersion -Version $PackageVersion
    $match = [regex]::Match($normalized, $script:FlitzZipSemVerPattern)
    return "$($match.Groups[1].Value).$($match.Groups[2].Value).$($match.Groups[3].Value)"
}

# Purpose: Refuse versions whose installer ordering would differ from the public release ordering.
# Inputs: PackageVersion is the requested release version. Outputs: Three bounded numeric fields or an error.
function Assert-FlitzZipMsiPackageVersion {
    param([Parameter(Mandatory = $true)][string]$PackageVersion)
    $normalized = Assert-FlitzZipPackageVersion -Version $PackageVersion
    if ($normalized.Contains('-')) {
        throw 'MSI versions require three numeric fields. Select the beta release track without a version suffix.'
    }
    $parts = $normalized.Split('.')
    $limits = @(255, 255, 65535)
    for ($index = 0; $index -lt 3; ++$index) {
        [uint32]$value = 0
        if (-not [uint32]::TryParse($parts[$index], [ref]$value) -or $value -gt $limits[$index]) {
            throw 'MSI versions must be within 255.255.65535; never truncate or wrap version fields.'
        }
    }
    return $normalized
}

# Purpose: Build the package artifact basename from a validated product version.
# Inputs: PackageVersion is a SemVer package version.
# Outputs: Returns the artifact basename without extension.
function Get-FlitzZipPackageBase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-FlitzZipPackageVersion -Version $PackageVersion
    return "FlitzZip-$normalized-win64"
}

# Purpose: Ensure package script inputs match the configured build tree before artifacts are created.
# Inputs: BuildRoot is the configured build directory; PackageVersion is the requested package version.
# Outputs: Returns no value; throws when cache or CPack metadata would produce mislabeled artifacts.
function Assert-FlitzZipPackageVersionMatchesBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,
        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    $normalized = Assert-FlitzZipPackageVersion -Version $PackageVersion
    $expectedPackageBase = Get-FlitzZipPackageBase -PackageVersion $normalized
    $expectedNumericVersion = Get-FlitzZipNumericPackageVersion -PackageVersion $normalized
    $buildVersion = Read-FlitzZipCMakeCacheValue -BuildRoot $BuildRoot -Name "FLITZZIP_PACKAGE_VERSION"
    if ($buildVersion -ne $normalized) {
        throw "Requested package version $normalized does not match configured build version $buildVersion. Re-run tools/build.ps1 with -PackageVersion $normalized before packaging."
    }
    $cpackFileName = Read-FlitzZipCPackValue -BuildRoot $BuildRoot -Name "CPACK_PACKAGE_FILE_NAME"
    if ($cpackFileName -ne $expectedPackageBase) {
        throw "CPack package file name $cpackFileName does not match expected $expectedPackageBase."
    }
    $cpackVersion = Read-FlitzZipCPackValue -BuildRoot $BuildRoot -Name "CPACK_PACKAGE_VERSION"
    if ($cpackVersion -ne $expectedNumericVersion) {
        throw "CPack package version $cpackVersion does not match expected numeric version $expectedNumericVersion."
    }
}
