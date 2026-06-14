param(
    [string]$Version = "7.0.0"
)

# Purpose: Install the pinned WiX CLI into the repository-local generated tool cache.
# Inputs: `Version` is the WiX dotnet-tool version required by release packaging.
# Outputs: Creates or updates `out\tools\wix\wix.exe`; does not accept the WiX OSMF EULA.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$toolPath = Join-Path $repo "out\tools\wix"

# Purpose: Find a dotnet host with an SDK, preferring the x64 installation on Windows.
# Inputs: None; probes known Windows x64 path and then PATH candidates.
# Outputs: Returns the dotnet executable path or throws when no SDK-capable host is available.
function Find-DotnetSdk {
    $candidates = @("C:\Program Files\dotnet\dotnet.exe")
    $commands = Get-Command dotnet -All -ErrorAction SilentlyContinue
    foreach ($command in $commands) {
        if ($command.Source -and ($candidates -notcontains $command.Source)) {
            $candidates += $command.Source
        }
    }
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate)) {
            continue
        }
        $sdks = & $candidate --list-sdks 2>$null
        if ($LASTEXITCODE -eq 0 -and $sdks) {
            return $candidate
        }
    }
    throw "No .NET SDK-capable dotnet host was found. Install the .NET SDK before installing WiX."
}

New-Item -ItemType Directory -Force -Path $toolPath | Out-Null
$dotnet = Find-DotnetSdk
$wix = Join-Path $toolPath "wix.exe"

if (Test-Path -LiteralPath $wix) {
    $installed = & $wix --version 2>$null
    if ($LASTEXITCODE -eq 0 -and $installed -like "$Version*") {
        Write-Host "WiX $installed is already installed at $wix"
        Write-Host "This script does not accept the WiX OSMF EULA."
        return
    }
    & $dotnet tool update --tool-path $toolPath wix --version $Version
} else {
    & $dotnet tool install --tool-path $toolPath wix --version $Version
}
if ($LASTEXITCODE -ne 0) {
    throw "WiX tool installation failed."
}
& $wix --version
Write-Host "Installed WiX $Version at $wix"
Write-Host "This script does not accept the WiX OSMF EULA."
