param(
    [ValidateRange(0, 2147483647)][int]$Runs = 512,
    [string]$Image = "gcr.io/oss-fuzz-base/base-builder@sha256:18973cd0446a865235843ccc4f6c561d99c27da9a2d32cd0ac55960a31e32975"
)

# Purpose: Build and smoke-run SuperZip libFuzzer targets inside the pinned ClusterFuzzLite base image.
# Inputs: `Runs` controls local fuzz iterations per target, and `Image` selects the pinned Docker image digest.
# Outputs: Writes fuzz artifacts under `out/fuzz`; fails on resource admission, build, sanitizer, or fuzzer errors.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'fuzz_resources.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo "out\fuzz"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$repoMount = ($repo -replace "\\", "/")
$outMount = ($outDir -replace "\\", "/")
$fuzzRuns = $Runs
$dockerAvailable = Get-SuperZipDockerAvailableMemoryMiB -Image $Image -RepoMount $repoMount
$memoryMiB = Resolve-SuperZipFuzzMemoryBudget -AvailableMiB (Get-SuperZipAvailableMemoryMiB) `
    -DockerAvailableMiB $dockerAvailable
$memoryLimit = "${memoryMiB}m"
Write-Output "fuzz_resource_admission memory_mib=$memoryMiB swap_mib=0 cpu_limit=none"

docker run --rm `
    --memory $memoryLimit --memory-swap $memoryLimit `
    -v "${repoMount}:/src:ro" `
    -v "${outMount}:/out" `
    -w /src `
    -e LIB_FUZZING_ENGINE=-fsanitize=fuzzer `
    -e OUT=/out `
    -e "SUPERZIP_FUZZ_MEMORY_MIB=$memoryMiB" `
    $Image `
    bash .clusterfuzzlite/local_smoke.sh $fuzzRuns
if ($LASTEXITCODE -ne 0) {
    throw "ClusterFuzzLite build or smoke run failed with exit code $LASTEXITCODE."
}
