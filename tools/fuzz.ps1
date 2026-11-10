param(
    [ValidateRange(0, 2147483647)][int]$Runs = 512,
    [string]$Image = "gcr.io/oss-fuzz-base/base-builder@sha256:18973cd0446a865235843ccc4f6c561d99c27da9a2d32cd0ac55960a31e32975",
    [ValidatePattern('^[a-f0-9]{32}$')][string]$OwnerId = [guid]::NewGuid().ToString('N'),
    [switch]$CleanupOnly
)

# Purpose: Build and smoke-run FlitzZip libFuzzer targets inside the pinned ClusterFuzzLite base image.
# Inputs: Runs bounds iterations; Image pins the toolchain; OwnerId identifies this operation's daemon resources.
# Outputs: Writes fuzz artifacts under out/fuzz and verifies cleanup; CleanupOnly recovers the recorded owner.
# A supervisor must retain OwnerId and run recovery if hard termination prevented this script's finally block.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'fuzz_resources.ps1')
if ($CleanupOnly) {
    if (-not $PSBoundParameters.ContainsKey('OwnerId')) { throw 'Fuzz recovery requires the recorded OwnerId.' }
    Remove-FlitzZipFuzzOwnedContainer -OwnerId $OwnerId
    return
}
Write-Output "fuzz_operation_owner=$OwnerId"
$repo = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo "out\fuzz"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$repoMount = ($repo -replace "\\", "/")
$outMount = ($outDir -replace "\\", "/")
$fuzzRuns = $Runs
$dockerAvailable = Get-FlitzZipDockerAvailableMemoryMiB -Image $Image -RepoMount $repoMount -OwnerId $OwnerId
$memoryMiB = Resolve-FlitzZipFuzzMemoryBudget -AvailableMiB (Get-FlitzZipAvailableMemoryMiB) `
    -DockerAvailableMiB $dockerAvailable
$memoryLimit = "${memoryMiB}m"
Write-Output "fuzz_resource_admission memory_mib=$memoryMiB swap_mib=0 cpu_limit=none"

try {
    docker run --rm --name "flitzzip-fuzz-$OwnerId-work" --label "io.flitzzip.fuzz-owner=$OwnerId" `
        --memory $memoryLimit --memory-swap $memoryLimit `
        -v "${repoMount}:/src:ro" `
        -v "${outMount}:/out" `
        -w /src `
        -e LIB_FUZZING_ENGINE=-fsanitize=fuzzer `
        -e OUT=/out `
        -e "FLITZZIP_FUZZ_MEMORY_MIB=$memoryMiB" `
        $Image `
        bash .clusterfuzzlite/local_smoke.sh $fuzzRuns
    if ($LASTEXITCODE -ne 0) {
        throw "ClusterFuzzLite build or smoke run failed with exit code $LASTEXITCODE."
    }
} finally { Remove-FlitzZipFuzzOwnedContainer -OwnerId $OwnerId }
