# Purpose: Run isolated Windows dependency memory-safety validation without altering the product build.
# Inputs: Installed MSVC x64 ASan and pinned repository CMake/dependency sources; no new runtime dependency.
# Outputs: Builds and runs canonical dependency contracts plus qualified instrumentation, failing on every tool error.
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
. (Join-Path $PSScriptRoot 'build_parallelism.ps1')

# Purpose: Bind sanitizer validation to the canonical native inputs and its own driver.
# Inputs: The current checkout; uses the existing bounded provenance inventory.
# Outputs: Returns portable input and driver identities or propagates inventory failure.
function Get-ZstdSanitizerInputSnapshot {
    $output = @(& py -3 (Join-Path $PSScriptRoot 'native_build_provenance.py') --root $repoRoot)
    $snapshotExit = $LASTEXITCODE
    if ($snapshotExit -ne 0) { throw "Sanitizer input capture failed ($snapshotExit)." }
    $snapshot = ($output -join "`n") | ConvertFrom-Json
    if ($snapshot.inputs_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Sanitizer input identity is invalid.' }
    return @{
        native = $snapshot
        driver_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$before = Get-ZstdSanitizerInputSnapshot
$validationRoot = Join-Path $repoRoot 'out/zstd-sanitizer-validation'
foreach ($path in @((Join-Path $repoRoot 'out'), $validationRoot)) {
    if ((Test-Path -LiteralPath $path) -and
        ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing a linked sanitizer validation directory.'
    }
}
$cmake = Find-CMake -RepoRoot $repoRoot
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$generator = Find-CMakeGenerator -Requested '' -BuildRoot $validationRoot
$jobs = Resolve-BuildParallelism
Invoke-SuperZipBackgroundWork {
    & $cmake -S (Join-Path $repoRoot 'tests/zstd/sanitizers') -B $validationRoot `
        -G $generator -A x64 "-DREPO_ROOT=$repoRoot"
    $configureExit = $LASTEXITCODE
    if ($configureExit -ne 0) { throw "Sanitizer configure failed ($configureExit)." }
    Invoke-SuperZipParallelBuild -Jobs $jobs -Action {
        & $cmake --build $validationRoot --config RelWithDebInfo --parallel $jobs
        $buildExit = $LASTEXITCODE
        if ($buildExit -ne 0) { throw "Sanitizer build failed ($buildExit)." }
    }
    & $ctest --test-dir $validationRoot -C RelWithDebInfo --output-on-failure
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { throw "Sanitizer dependency contracts failed ($testExit)." }
}
$after = Get-ZstdSanitizerInputSnapshot
if ($before.native.inputs_sha256 -ne $after.native.inputs_sha256 -or
    $before.driver_sha256 -ne $after.driver_sha256) {
    throw 'Sanitizer inputs changed during validation; no successful receipt will be written.'
}
$receipt = @{
    schema_version = 1; status = 'passed'; inputs = $after
    scope = 'isolated-msvc-x64-asan-dependency-contracts-not-product-hip-acceptance'
    configuration = 'RelWithDebInfo'; completed_utc = [DateTime]::UtcNow.ToString('o')
    library_sha256 = (Get-FileHash -LiteralPath (Join-Path $validationRoot 'RelWithDebInfo/libzstd.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$receiptPath = Join-Path $validationRoot ('qualification-' + [guid]::NewGuid().ToString('N') + '.json')
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
Write-Output 'Isolated MSVC AddressSanitizer dependency validation passed; this is not product HIP qualification.'
Write-Output "Sanitizer receipt inputs_sha256=$($after.native.inputs_sha256)"
