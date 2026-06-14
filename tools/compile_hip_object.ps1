param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,
    [string]$Arch = "gfx1201",
    [string]$VcvarsVersion = "14.44"
)

$ErrorActionPreference = "Stop"

if (-not $env:HIP_PATH) {
    throw "HIP_PATH is not set."
}

$hipcc = Join-Path $env:HIP_PATH "bin\hipcc.exe"
if (-not (Test-Path $hipcc)) {
    throw "hipcc.exe not found at $hipcc"
}

$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    throw "vcvarsall.bat was not found."
}

$outputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$include = Join-Path $RepoRoot "src"
$cmd = "call `"$vcvars`" amd64 -vcvars_ver=$VcvarsVersion >nul && `"$hipcc`" --offload-arch=$Arch -std=c++20 -O3 -fms-runtime-lib=static -DSUPERZIP_ENABLE_HIP=1 -I`"$include`" -c `"$Source`" -o `"$Output`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "hipcc failed with exit code $LASTEXITCODE"
}
