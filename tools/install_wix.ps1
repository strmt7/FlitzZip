param(
    [ValidateSet("7.0.1")][string]$Version = "7.0.1"
)

# Purpose: Install the pinned WiX CLI into the repository-local generated tool cache.
# Inputs: `Version` is the WiX dotnet-tool version required by release packaging.
# Outputs: Creates a versioned checkout-owned installation; does not accept the WiX OSMF EULA.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "package_runtime.ps1")
$tool = Get-FlitzZipWixToolchain -RepoRoot $repo
if ($Version -cne $tool.Version) { throw "WiX installer and shared toolchain pins disagree." }
$toolPath = Split-Path -Parent $tool.Path

# Purpose: Find a dotnet host with an SDK, preferring the x64 installation on Windows.
# Inputs: None; probes Windows' Program Files folder and then PATH candidates.
# Outputs: Returns the dotnet executable path or throws when no SDK-capable host is available.
function Find-DotnetSdk {
    $candidates = @()
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    if ($programFiles) {
        $candidates += (Join-Path $programFiles "dotnet\dotnet.exe")
    }
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

if (Test-Path -LiteralPath $tool.Path) {
    $wix = Resolve-FlitzZipWix -RepoRoot $repo
    Write-Output "WiX $Version is already installed at $wix"
    return
}
if (Test-Path -LiteralPath $toolPath) {
    throw "Incomplete WiX installation at $toolPath; inspect it before retrying."
}
$dotnet = Find-DotnetSdk
foreach ($directory in @((Join-Path $repo 'out'), (Join-Path $repo 'out/tools'))) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing a redirected WiX installation parent.'
    }
}
New-Item -ItemType Directory -Path $toolPath | Out-Null
# Clear inherited package feeds rather than letting a private/user source supply this tool.
$config = Join-Path $toolPath 'NuGet.Config'
[IO.File]::WriteAllText($config, '<configuration><packageSources><clear/><add key="nuget.org" value="https://api.nuget.org/v3/index.json" /></packageSources></configuration>')
& $dotnet tool install --tool-path $toolPath wix --version $Version --configfile $config
if ($LASTEXITCODE -ne 0) { throw "WiX tool installation failed; the incomplete cache is retained at $toolPath." }
$wix = Resolve-FlitzZipWix -RepoRoot $repo
Write-Output "Installed qualified WiX $Version at $wix"
Write-Output "This script does not accept the WiX OSMF EULA."
