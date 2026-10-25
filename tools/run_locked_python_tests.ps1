param(
    [Parameter(Mandatory = $true)][string]$Modules
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lock = Join-Path $repo '.github/requirements/requirements-contracts.txt'
$names = @($Modules.Split(',') | Where-Object { $_ -ne '' })
if ($names.Count -eq 0 -or @($names | Where-Object { $_ -notmatch '^tools\.test_[a-z0-9_]+$' }).Count -ne 0) {
    throw 'Locked Python test modules must be explicit tools.test_* names.'
}

# Purpose: Select an installed supported interpreter without depending on one Python installation path.
# Inputs: Optional explicit FLITZZIP_PYTHON, then registered launcher and PATH candidates.
# Outputs: An executable, argv prefix and exact interpreter version, or a prerequisite error.
function Find-ContractPython {
    $candidates = @()
    if ($env:FLITZZIP_PYTHON) {
        if (-not (Test-Path -LiteralPath $env:FLITZZIP_PYTHON)) {
            throw 'FLITZZIP_PYTHON does not name an installed Python executable.'
        }
        $candidates += [pscustomobject]@{ File = $env:FLITZZIP_PYTHON; Prefix = @() }
    } else {
        $launcher = Get-Command py -ErrorAction SilentlyContinue
        if ($null -ne $launcher) {
            foreach ($version in @('3.13', '3.14')) {
                $candidates += [pscustomobject]@{ File = $launcher.Source; Prefix = @("-$version") }
            }
        }
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -ne $python) {
            $candidates += [pscustomobject]@{ File = $python.Source; Prefix = @() }
        }
    }
    foreach ($candidate in $candidates) {
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $version = & $candidate.File @($candidate.Prefix) -c 'import sys; print(sys.version_info.major, sys.version_info.minor)' 2>$null
            $status = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($status -eq 0 -and $version -in @('3 13', '3 14')) {
            return [pscustomobject]@{ File = $candidate.File; Prefix = @($candidate.Prefix); Version = $version.Replace(' ', '.') }
        }
    }
    throw 'Install CPython 3.13 or 3.14, or set FLITZZIP_PYTHON to one of those interpreters.'
}

# Purpose: Create only ordinary cache directories below the checkout.
# Inputs: Parent directory and one child name. Outputs: Child path; existing reparse points are refused.
function Add-OrdinaryDirectory {
    param([string]$Parent, [string]$Name)
    $child = Join-Path $Parent $Name
    if (Test-Path -LiteralPath $child) {
        $item = Get-Item -LiteralPath $child -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Python contract cache crosses a reparse point or file.'
        }
    } else {
        New-Item -ItemType Directory -Path $child | Out-Null
    }
    return $child
}

$selected = Find-ContractPython
$lockHash = (Get-FileHash -LiteralPath $lock -Algorithm SHA256).Hash
$pin = [regex]::Match((Get-Content -LiteralPath $lock -Raw), '(?m)^packaging==([0-9.]+)\s*\\')
if (-not $pin.Success) { throw 'The locked Python contract dependency is missing.' }
$tools = Add-OrdinaryDirectory (Add-OrdinaryDirectory $repo 'out') 'tools'
$base = Add-OrdinaryDirectory $tools 'python-contracts'
$target = Add-OrdinaryDirectory $base ("py{0}-{1}" -f $selected.Version.Replace('.', ''), $lockHash.Substring(0, 12))
$venvPython = Join-Path $target 'Scripts/python.exe'
if (-not (Test-Path -LiteralPath $venvPython)) {
    & $selected.File @($selected.Prefix) -m venv $target
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create the isolated Python contract environment.' }
}
$stamp = Join-Path $target 'requirements.sha256'
$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $installed = & $venvPython -c 'import packaging; print(packaging.__version__)' 2>$null
    $installedStatus = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousPreference
}
if ($installedStatus -ne 0 -or $installed -ne $pin.Groups[1].Value -or -not (Test-Path -LiteralPath $stamp) -or
    (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $lockHash) {
    & $venvPython -m pip install --disable-pip-version-check --require-hashes --only-binary=:all: -r $lock
    if ($LASTEXITCODE -ne 0) { throw 'Hash-locked Python contract dependency installation failed.' }
    $installed = & $venvPython -c 'import packaging; print(packaging.__version__)'
    if ($LASTEXITCODE -ne 0 -or $installed -ne $pin.Groups[1].Value) {
        throw 'Installed Python contract dependency differs from the lock.'
    }
    Set-Content -LiteralPath $stamp -Value $lockHash -Encoding ascii
}
& $venvPython -B -m unittest @names
if ($LASTEXITCODE -ne 0) { throw 'Locked Python contract tests failed.' }
