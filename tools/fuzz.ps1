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
mkdir -p /out/corpus/archive_index /out/corpus/path_safety /out/corpus/iso /out/corpus/cab /out/corpus/rpm /out/corpus/sevenzip /out/corpus/lha
printf 'SUZP\001\000\000\000\000\000\000\000' > /out/corpus/archive_index/empty-index
printf '../escape' > /out/corpus/path_safety/traversal
printf 'C:/absolute' > /out/corpus/path_safety/drive-rooted
printf 'safe/nested/file.txt' > /out/corpus/path_safety/safe-relative
printf 'CD001' > /out/corpus/iso/tiny-cd001
printf 'MSCF' > /out/corpus/cab/tiny-mscf
printf '\355\253\356\333\003\000' > /out/corpus/rpm/tiny-rpm-lead
printf '7z\274\257\047\034\000\003' > /out/corpus/sevenzip/tiny-sevenzip-signature
printf '\031\216-lhd-' > /out/corpus/lha/tiny-lha-signature
python3 - <<'PY'
from pathlib import Path
Path('/out/corpus/sevenzip/nested-payload.7z').write_bytes(bytes([
    55, 122, 188, 175, 39, 28, 0, 3, 61, 67, 90, 149, 110, 0, 0, 0,
    0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 83, 152, 7, 164,
    1, 0, 15, 115, 101, 118, 101, 110, 122, 105, 112, 32, 112, 97, 121, 108,
    111, 97, 100, 0, 0, 0, 129, 51, 7, 174, 15, 207, 57, 176, 12, 7,
    200, 67, 127, 65, 177, 250, 253, 226, 251, 121, 219, 32, 43, 173, 94, 44,
    42, 8, 17, 64, 221, 175, 147, 38, 76, 135, 221, 114, 36, 255, 78, 89,
    238, 232, 52, 84, 176, 173, 57, 39, 80, 178, 173, 141, 182, 201, 248, 140,
    45, 225, 25, 37, 113, 224, 222, 155, 40, 199, 58, 161, 53, 45, 38, 9,
    204, 203, 200, 225, 204, 104, 40, 134, 221, 252, 239, 51, 192, 0, 23, 6,
    20, 1, 9, 90, 0, 7, 11, 1, 0, 1, 35, 3, 1, 1, 5, 93,
    0, 0, 64, 0, 12, 94, 10, 1, 167, 128, 3, 7, 0, 0,
]))
Path('/out/corpus/lha/nested-payload.lzh').write_bytes(bytes([
    25, 142, 45, 108, 104, 100, 45, 29, 0, 0, 0, 0, 0, 0, 0, 233,
    163, 152, 64, 32, 1, 0, 0, 0, 85, 5, 0, 80, 192, 65, 7, 0,
    81, 232, 3, 232, 3, 10, 0, 2, 115, 117, 98, 100, 105, 114, 255, 7,
    0, 84, 135, 255, 150, 79, 0, 0, 25, 150, 45, 108, 104, 100, 45, 37,
    0, 0, 0, 0, 0, 0, 0, 233, 163, 152, 64, 32, 1, 0, 0, 0,
    85, 5, 0, 80, 109, 65, 7, 0, 81, 232, 3, 232, 3, 18, 0, 2,
    115, 117, 98, 100, 105, 114, 255, 115, 117, 98, 100, 105, 114, 50, 255, 7,
    0, 84, 135, 255, 150, 79, 0, 0, 34, 45, 45, 108, 104, 48, 45, 49,
    0, 0, 0, 12, 0, 0, 0, 0, 0, 33, 60, 32, 1, 9, 104, 101,
    108, 108, 111, 46, 116, 120, 116, 120, 151, 85, 5, 0, 80, 164, 129, 7,
    0, 81, 232, 3, 232, 3, 18, 0, 2, 115, 117, 98, 100, 105, 114, 255,
    115, 117, 98, 100, 105, 114, 50, 255, 7, 0, 84, 0, 59, 61, 75, 0,
    0, 104, 101, 108, 108, 111, 32, 119, 111, 114, 108, 100, 10, 0,
]))
PY
if [ "$fuzzRuns" -gt 0 ]; then
  /out/superzip_archive_index_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/archive_index
  /out/superzip_path_safety_fuzzer -runs=$fuzzRuns -max_len=4096 /out/corpus/path_safety
  /out/superzip_iso_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/iso
  /out/superzip_cab_header_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/cab
  /out/superzip_rpm_header_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/rpm
  /out/superzip_sevenzip_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/sevenzip
  /out/superzip_lha_fuzzer -runs=$fuzzRuns -max_len=1048576 /out/corpus/lha
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
