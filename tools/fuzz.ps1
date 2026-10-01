param(
    [int]$Runs = 512,
    [string]$Image = "gcr.io/oss-fuzz-base/base-builder@sha256:18973cd0446a865235843ccc4f6c561d99c27da9a2d32cd0ac55960a31e32975"
)

# Purpose: Build and smoke-run SuperZip libFuzzer targets inside the pinned ClusterFuzzLite base image.
# Inputs: `Runs` controls local fuzz iterations per target, and `Image` selects the pinned Docker image digest.
# Outputs: Writes fuzz binaries/artifacts under `out/fuzz`; Docker exits nonzero on build, sanitizer, or fuzzer failure.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo "out\fuzz"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$repoMount = ($repo -replace "\\", "/")
$outMount = ($outDir -replace "\\", "/")
$fuzzRuns = [Math]::Max(0, $Runs)

docker run --rm `
    -v "${repoMount}:/src:ro" `
    -v "${outMount}:/out" `
    -w /src `
    -e LIB_FUZZING_ENGINE=-fsanitize=fuzzer `
    -e OUT=/out `
    $Image `
    bash .clusterfuzzlite/local_smoke.sh $fuzzRuns
if ($LASTEXITCODE -ne 0) {
    throw "ClusterFuzzLite build or smoke run failed with exit code $LASTEXITCODE."
}
