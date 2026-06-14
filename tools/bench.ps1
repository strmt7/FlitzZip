param(
    [string]$Configuration = "Release",
    [switch]$RequireGpu
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
if (-not (Test-Path $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}

$work = Join-Path $env:TEMP ("superzip-bench-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $input = Join-Path $work "input"
    New-Item -ItemType Directory -Force -Path $input | Out-Null
    $data = New-Object byte[] (64MB)
    for ($i = 0; $i -lt $data.Length; $i++) {
        $data[$i] = [byte](($i * 131) -band 255)
    }
    [IO.File]::WriteAllBytes((Join-Path $input "pattern.bin"), $data)
    $gpuFlag = @()
    if ($RequireGpu) {
        $gpuFlag = @("--require-gpu")
    }
    & $cli compress --format szip @gpuFlag --output (Join-Path $work "bench.szip") $input
    & $cli verify (Join-Path $work "bench.szip")
    & $cli extract --format szip @gpuFlag --output (Join-Path $work "out") (Join-Path $work "bench.szip")
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
