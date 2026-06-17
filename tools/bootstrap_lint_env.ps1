param(
    [string]$PythonPath = $env:SUPERZIP_PYTHON,
    [string]$VenvPath = "",
    [switch]$SkipPowerShellAnalyzer
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($VenvPath)) {
    $VenvPath = Join-Path $repo ".venv-lint"
}

$requirementsPath = Join-Path $repo ".github/requirements/requirements-lint-windows.txt"
$localModuleRoot = Join-Path $repo ".psmodules"

# Purpose: Resolve the Python executable used to create the lint virtual environment.
# Inputs: `RequestedPath` may name a Python executable supplied by the caller or environment.
# Outputs: Returns a command path and optional launcher arguments, or throws when Python is unavailable.
function Resolve-BootstrapPython {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath)) {
            throw "Requested Python executable does not exist: $RequestedPath"
        }
        return [pscustomobject]@{ Command = $RequestedPath; PrefixArgs = @() }
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -ne $python) {
        return [pscustomobject]@{ Command = $python.Source; PrefixArgs = @() }
    }

    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $pyLauncher) {
        return [pscustomobject]@{ Command = $pyLauncher.Source; PrefixArgs = @("-3") }
    }

    throw "Python was not found. Install Python 3.12+ or set SUPERZIP_PYTHON to a Python executable."
}

# Purpose: Invoke the resolved bootstrap Python command and fail on non-zero exit.
# Inputs: `Python` is the resolved command object and `Arguments` are passed to Python.
# Outputs: Throws when the Python command fails.
function Invoke-BootstrapPython {
    param(
        [Parameter(Mandatory = $true)]$Python,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Python.Command @($Python.PrefixArgs + $Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "Python bootstrap command failed with exit code $LASTEXITCODE."
    }
}

# Purpose: Install the pinned Python linter requirements into the local virtual environment.
# Inputs: `Python` is the resolved host Python used only to create the venv.
# Outputs: Creates or refreshes `.venv-lint` with hash-locked linter packages.
function Install-PythonLinterEnvironment {
    param([Parameter(Mandatory = $true)]$Python)

    $venvPython = Join-Path $VenvPath "Scripts/python.exe"
    if (-not (Test-Path -LiteralPath $venvPython)) {
        Invoke-BootstrapPython -Python $Python -Arguments @("-m", "venv", $VenvPath)
    }
    & $venvPython -m pip install `
        --disable-pip-version-check `
        --require-hashes `
        --only-binary=:all: `
        -r $requirementsPath
    if ($LASTEXITCODE -ne 0) {
        throw "Pinned Python linter installation failed with exit code $LASTEXITCODE."
    }
}

# Purpose: Install the pinned PSScriptAnalyzer module without PackageManagement bootstrap.
# Inputs: None; uses the version and SHA-256 pinned to the lint workflow.
# Outputs: Extracts the module into `.psmodules` when it is not already present.
function Install-PinnedPSScriptAnalyzer {
    $moduleName = "PSScriptAnalyzer"
    $version = "1.24.0"
    $expectedSha256 = "E86C97D44BB1BC8A1DE35E753B85EA1D938F6F9F881639A181507E079BCA4556"
    $modulePath = Join-Path $localModuleRoot "$moduleName/$version"
    $manifest = Join-Path $modulePath "$moduleName.psd1"
    if (Test-Path -LiteralPath $manifest) {
        return
    }

    $package = Join-Path ([System.IO.Path]::GetTempPath()) "$moduleName.$version.nupkg"
    Invoke-WebRequest `
        -Uri "https://www.powershellgallery.com/api/v2/package/$moduleName/$version" `
        -OutFile $package
    $actualSha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    if ($actualSha256 -ne $expectedSha256) {
        throw "$moduleName $version package hash mismatch."
    }

    if (Test-Path -LiteralPath $modulePath) {
        Remove-Item -LiteralPath $modulePath -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $modulePath | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($package, $modulePath)
}

Push-Location $repo
try {
    $python = Resolve-BootstrapPython -RequestedPath $PythonPath
    Install-PythonLinterEnvironment -Python $python
    if (-not $SkipPowerShellAnalyzer.IsPresent) {
        Install-PinnedPSScriptAnalyzer
    }
    Write-Output "lint bootstrap complete"
    Write-Output "venv=$VenvPath"
    Write-Output "psmodules=$localModuleRoot"
} finally {
    Pop-Location
}
