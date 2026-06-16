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

$script = @"
set -euo pipefail
mkdir -p /out
bash .clusterfuzzlite/build.sh
rm -rf /out/corpus
mkdir -p /out/corpus/archive_index /out/corpus/path_safety /out/corpus/iso /out/corpus/rpm
printf 'SUZP\001\000\000\000\000\000\000\000' > /out/corpus/archive_index/empty-index
printf '../escape' > /out/corpus/path_safety/traversal
printf 'C:/absolute' > /out/corpus/path_safety/drive-rooted
printf 'safe/nested/file.txt' > /out/corpus/path_safety/safe-relative
printf 'CD001' > /out/corpus/iso/tiny-cd001
printf '\355\253\356\333\003\000' > /out/corpus/rpm/tiny-rpm-lead
if [ "$fuzzRuns" -gt 0 ]; then
  /out/superzip_archive_index_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/archive_index
  /out/superzip_path_safety_fuzzer -runs=$fuzzRuns -max_len=4096 /out/corpus/path_safety
  /out/superzip_iso_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/iso
  /out/superzip_rpm_header_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/rpm
fi
"@

docker run --rm `
    -v "${repoMount}:/src:ro" `
    -v "${outMount}:/out" `
    -w /src `
    -e LIB_FUZZING_ENGINE=-fsanitize=fuzzer `
    -e OUT=/out `
    $Image `
    bash -lc $script
