$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native_test_selection.ps1')
$root = Split-Path -Parent $PSScriptRoot

# Purpose: Reject a violated production-selection contract with a specific diagnostic.
# Inputs: Condition describes the expected behavior; Message labels the affected boundary.
# Outputs: Throws on a false condition.
function Assert-NativeSelection {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Native selection contract failed: $Message" }
}

# Purpose: Require a rejected production operation without executing unrelated product workloads.
# Inputs: Action invokes the production selector or executor; Expected is the relevant error fragment.
# Outputs: Fails if accepted or if the original diagnostic is lost.
function Assert-NativeSelectionRejected {
    param([scriptblock]$Action, [string]$Expected)
    $cause = ''
    try { & $Action | Out-Null } catch { $cause = $_.Exception.Message }
    Assert-NativeSelection ($cause.Contains($Expected)) "Expected rejection '$Expected', received '$cause'"
}

$entropyPaths = @('src/gpu/hip_codec_static_prefix.hip.cpp', 'src/gpu/hip_codec_adaptive_prefix.hip.cpp')
foreach ($inputPaths in @(@('tests/cpp/test_dictionary_block.cpp'), $entropyPaths)) {
    $pathJson = ConvertTo-Json -InputObject $inputPaths -Compress
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pathJson))
    $decoded = @(ConvertFrom-FlitzZipNativeSelectionPath -Base64 $encoded)
    Assert-NativeSelection (($decoded -join '|') -eq ($inputPaths -join '|')) 'single/multiple path array decoding'
}
foreach ($invalidJson in @('"tests/cpp/test_dictionary_block.cpp"', '[]', '[""]', '["../test.cpp"]', '[{}]', '["C:/test.cpp"]')) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($invalidJson))
    Assert-NativeSelectionRejected { ConvertFrom-FlitzZipNativeSelectionPath -Base64 $encoded } 'Native selection requires'
}
$entropy = Get-FlitzZipNativeTestSelection -Paths $entropyPaths
$entropyExpected = @(
    'flzip_gpu_efforts_preserve_dictionary_candidates_after_entropy_gain',
    'flzip_gpu_entropy_efforts_preserve_per_block_winners',
    'flzip_gpu_huffman_corruption_is_rejected', 'flzip_gpu_huffman_lookup_requires_exact_extent',
    'flzip_gpu_huffman_replaces_static_prefix_when_smaller',
    'flzip_gpu_huffman_sparse_lookup_validation', 'flzip_gpu_prefix_candidate_selection_is_per_block',
    'flzip_gpu_prefix_levels_compact_shifted_alphabet', 'flzip_gpu_prefix_malformed_codewords_are_rejected',
    'flzip_gpu_prefix_packing_matches_reference', 'flzip_gpu_prefix_segment_boundaries_roundtrip',
    'flzip_prefix_reference_cpu_decode_covers_every_symbol', 'flzip_prefix_reference_known_bytes',
    'flzip_required_gpu_huffman_blocks_honor_compression_level',
    'flzip_required_gpu_prefix_blocks_compress_low_entropy_payload',
    'flzip_required_gpu_prefix_blocks_compress_raw_blocks_inside_mixed_chunk',
    'flzip_required_gpu_prefix_payload_corruption_is_rejected',
    'flzip_required_gpu_prefix_table_corruption_is_rejected', 'gpu_block_batch_hip_identity'
)
Assert-NativeSelection ($entropy.mode -eq 'component' -and $entropy.requireHip -and
    (($entropy.tests | Sort-Object) -join '|') -ceq (($entropyExpected | Sort-Object) -join '|')) 'exact reviewed entropy consumers and required HIP'
Assert-NativeSelection ('flzip_gpu_entropy_efforts_preserve_per_block_winners' -in $entropy.tests -and
    'flzip_gpu_efforts_preserve_dictionary_candidates_after_entropy_gain' -in $entropy.tests) 'direct encoder integration consumers'
Assert-NativeSelection ('flzip_gpu_mixed_materialization_skips_unaligned_prefix_windows' -notin $entropy.tests) 'unrelated manual decoder fixture exclusion'
$sparsePaths = @('src/gpu/sparse_pattern_candidate.cpp', 'src/gpu/hip_sparse_pattern.hip.cpp')
$sparse = Get-FlitzZipNativeTestSelection -Paths $sparsePaths
Assert-NativeSelection ($sparse.mode -eq 'component' -and $sparse.requireHip -and $sparse.tests.Count -eq 12) 'reviewed sparse candidate cohort and required HIP'
foreach ($name in @('sparse_pattern_hip_batch_rejects_prepopulated_output_fields',
        'sparse_pattern_hip_batch_preserves_block_offsets', 'long_sparse_pattern_required_hip_roundtrip',
        'flzip_sparse_pattern_cpu_reader_roundtrip', 'flzip_long_sparse_pattern_v7_reader_roundtrip',
        'flzip_long_sparse_pattern_writer_selects_v7')) {
    Assert-NativeSelection ($name -in $sparse.tests) "sparse direct admission, layout and archive consumer coverage: $name"
}
$gpuBatch = Get-FlitzZipNativeTestSelection -Paths ($entropyPaths + $sparsePaths)
$expectedBatch = @($entropyExpected + $sparse.tests | Sort-Object -Unique)
Assert-NativeSelection ($gpuBatch.requireHip -and
    (($gpuBatch.tests | Sort-Object) -join '|') -ceq ($expectedBatch -join '|')) 'entropy/sparse batch preserves every consumer without repetition'
$dictionaryPaths = @('src/gpu/dictionary_candidate.cpp')
$dictionary = Get-FlitzZipNativeTestSelection -Paths $dictionaryPaths
Assert-NativeSelection ($dictionary.mode -eq 'component' -and $dictionary.requireHip -and $dictionary.tests.Count -eq 15) 'reviewed dictionary candidate cohort and required HIP'
foreach ($name in @('dictionary_off_grid_segmented_records_roundtrip',
        'dictionary_adjacent_periodic_blocks_preserve_encoded_bytes', 'dictionary_production_chunk_crc_and_decode',
        'flzip_gpu_dictionary_writer_levels_and_roundtrip', 'flzip_required_gpu_encoder_emits_no_cpu_deflate_blocks',
        'flzip_gpu_efforts_preserve_dictionary_candidates_after_entropy_gain')) {
    Assert-NativeSelection ($name -in $dictionary.tests) "dictionary admission, effort, grouping and publication coverage: $name"
}
Assert-NativeSelection ('dictionary_encoder_benchmark_opt_in' -notin $dictionary.tests -and
    'dictionary_decoder_independent_writer_fixture' -notin $dictionary.tests) 'candidate changes exclude opt-in timing and standalone decoder fixtures'
$allCandidates = Get-FlitzZipNativeTestSelection -Paths ($entropyPaths + $sparsePaths + $dictionaryPaths)
$dictionaryCodec = Get-FlitzZipNativeTestSelection -Paths @('src/gpu/hip_dictionary_matcher.hip.cpp')
Assert-NativeSelection ($dictionaryCodec.mode -eq 'component' -and $dictionaryCodec.requireHip) 'reviewed dictionary codec requires its component contracts and HIP'
foreach ($name in @('dictionary_gpu_stable_prefix_order_with_maximum_sentinels',
        'dictionary_gpu_matches_agree_with_exhaustive_reference', 'dictionary_encoder_agrees_with_dense_reference',
        'dictionary_gpu_maximum_batch_is_bounded', 'dictionary_decoder_rejects_incomplete_sequences',
        'dictionary_concurrent_dense_match_table', 'flzip_gpu_dictionary_writer_levels_and_roundtrip')) {
    Assert-NativeSelection ($name -in $dictionaryCodec.tests) "dictionary codec sort/search, decoder, concurrency and direct publication coverage: $name"
}
foreach ($name in @('dictionary_encoder_benchmark_opt_in', 'dictionary_sparse_pattern_reference_opt_in',
        'dictionary_interop_export_preserves_bytes_and_refuses_overwrite')) {
    Assert-NativeSelection ($name -notin $dictionaryCodec.tests) "codec changes do not select unrelated opt-in research or file-export fixtures: $name"
}
$expectedCandidates = @($entropyExpected + $sparse.tests + $dictionary.tests | Sort-Object -Unique)
Assert-NativeSelection ($allCandidates.requireHip -and
    (($allCandidates.tests | Sort-Object) -join '|') -ceq ($expectedCandidates -join '|')) 'candidate batch preserves every consumer without repetition'
foreach ($shared in @('src/gpu/dictionary_candidate.hpp', 'src/gpu/dictionary_matcher.cpp', 'src/gpu/dictionary_device.hpp')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths ($dictionaryPaths + $shared)).mode -eq 'full') "unmapped dictionary boundary retains broader coverage: $shared"
}
Assert-NativeSelection ('gpu_block_batch_hip_identity' -notin $sparse.tests -and
    'flzip_gpu_prefix_packing_matches_reference' -notin $sparse.tests) 'sparse changes exclude unrelated encoder and standalone batch fixtures'
foreach ($shared in @('src/gpu/sparse_pattern_candidate.hpp', 'src/gpu/sparse_pattern_device.hpp',
        'src/core/sparse_pattern_block.cpp')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths ($sparsePaths + $shared)).mode -eq 'full') "unmapped sparse shared/parser input retains broader coverage: $shared"
}
$repeated = Get-FlitzZipNativeTestSelection -Paths ($entropyPaths + $entropyPaths + 'docs/design.md')
Assert-NativeSelection (($repeated.tests -join '|') -eq ($entropy.tests -join '|')) 'batch union deduplicates cases'
foreach ($shared in @('src/gpu/hip_codec_support.hpp', 'src/gpu/hip_codec.hip.cpp', 'CMakeLists.txt',
        'tests/cpp/test_main.cpp', 'tests/cpp/test_util.hpp', 'tests/cpp/test_zstd_bounds.cpp')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths ($entropyPaths + $shared)).mode -eq 'full') "unmapped/shared inputs retain broad coverage: $shared"
}
Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths $entropyPaths -Full).mode -eq 'full') 'explicit full qualification'
Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_main.cpp')).mode -eq 'runner') 'runner-only edits select registry contracts'
$inventory = @(Get-FlitzZipNativeTestInventory)
$sevenzip = Get-FlitzZipNativeTestSelection -Paths @('third_party/lzma_sdk/C/7zArcIn.c')
$sevenzipExpected = @($inventory | Where-Object { $_.source -in @('tests/cpp/test_sevenzip_compat.cpp',
        'tests/cpp/test_sevenzip_ppmd.cpp') } | Select-Object -ExpandProperty name | Sort-Object)
Assert-NativeSelection ($sevenzip.mode -eq 'component' -and -not $sevenzip.requireHip -and
    ($sevenzip.tests -join '|') -ceq ($sevenzipExpected -join '|')) '7z header parser selects all registered parser and decoder consumers'
foreach ($shared in @('third_party/lzma_sdk/C/7z.h', 'third_party/lzma_sdk/C/LzmaDec.c', 'third_party/lzma_sdk/C/CpuArch.h')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths @('third_party/lzma_sdk/C/7zArcIn.c', $shared)).mode -eq 'full') "shared SDK changes retain broader coverage: $shared"
}
$testPath = 'tests/cpp/test_dictionary_block.cpp'
$testOnly = Get-FlitzZipNativeTestSelection -Paths @($testPath)
$expected = @($inventory | Where-Object source -eq $testPath | Select-Object -ExpandProperty name | Sort-Object)
Assert-NativeSelection ($testOnly.mode -eq 'component' -and -not $testOnly.requireHip -and
    ($testOnly.tests -join '|') -eq ($expected -join '|')) 'registered changed test source coverage'
$combined = Get-FlitzZipNativeTestSelection -Paths ($entropyPaths + $testPath)
Assert-NativeSelection ($combined.requireHip -and $combined.tests.Count -eq ($entropy.tests.Count + $expected.Count)) 'distinct mechanisms form one deduplicated batch'

$ownedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/native-selection-tests'))
$fixtureRoot = Join-Path $ownedRoot ([guid]::NewGuid().ToString('N'))
$fixtureTests = Join-Path $fixtureRoot 'tests/cpp'
New-Item -ItemType Directory -Path $fixtureTests -Force | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)')
    $fixtureSource = Join-Path $fixtureTests 'test_fixture.cpp'
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(sample_longer) {}")
    $overlapping = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_fixture.cpp') -Root $fixtureRoot
    Assert-NativeSelection ($overlapping.tests.Count -eq 2) 'overlapping names remain distinct exact cases'
    foreach ($conditional in @("#if FLITZZIP_ENABLE_HIP`nTEST_CASE(sample) {}`n#endif",
            "#ifdef _WIN32`n#if FLITZZIP_ENABLE_HIP`nTEST_CASE(sample) {}`n#endif`n#endif",
            "#ifdef _WIN32`n#else`nTEST_CASE(sample) {}`n#endif")) {
        [IO.File]::WriteAllText($fixtureSource, $conditional)
        Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'unsupported build condition'
    }
    foreach ($portable in @("#ifdef _WIN32`nTEST_CASE(sample) {}`n#endif",
            "#if defined(_WIN32)`nTEST_CASE(sample) {}`n#endif",
            "TEST_CASE(sample) {`n#if FLITZZIP_ENABLE_HIP`n// optional body`n#endif`n}")) {
        [IO.File]::WriteAllText($fixtureSource, $portable)
        Assert-NativeSelection (@(Get-FlitzZipNativeTestInventory -Root $fixtureRoot).Count -eq 1) 'Windows registration and build-conditional bodies remain compatible'
    }
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`n  TEST_CASE(indented) {}")
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'unsupported registration syntax'
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(sample) {}")
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'Duplicate native test registration'
    [IO.File]::WriteAllText($fixtureSource, '// no registrations')
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'no recognized cases'
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(different_target OBJECT test.cpp)')
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'Cannot resolve'
} finally {
    $prefix = $ownedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixtureRoot).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified native-selection fixture.'
    }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}

$script:NativeFixtureCalls = [Collections.Generic.List[string]]::new()
$script:NativeFixtureMode = 'pass'
$script:NativeFixtureHipMode = 'ready'

# Purpose: Supply controlled CLI readiness output to the actual component executor.
# Inputs: Argument must be gpu-info; fixture state can reject before or after execution.
# Outputs: Records the call and returns the declared compiled/available HIP state.
function Invoke-NativeFixtureCli {
    param([string]$Argument)
    $script:NativeFixtureCalls.Add("cli:$Argument")
    $global:LASTEXITCODE = if ($script:NativeFixtureHipMode -eq 'cpu-only') { 1 }
        elseif ($script:NativeFixtureHipMode -eq 'cpu-error') { 2 } else { 0 }
    $checks = @($script:NativeFixtureCalls | Where-Object { $_ -eq 'cli:gpu-info' }).Count
    if ($script:NativeFixtureHipMode -in @('not-compiled', 'cpu-only', 'cpu-success', 'cpu-error')) { 'hip_compiled=false' } else { 'hip_compiled=true' }
    if ($script:NativeFixtureHipMode -in @('unavailable', 'cpu-only', 'cpu-success', 'cpu-error') -or ($script:NativeFixtureHipMode -eq 'lost' -and $checks -gt 1)) {
        'available=false'
    } else { 'available=true' }
}

# Purpose: Exercise exact success/failure parsing and early-stop behavior without running native workloads.
# Inputs: Name is the requested case; fixture mode alters its process status or registry output.
# Outputs: Records one invocation and emits controlled runner output and exit status.
function Invoke-NativeFixtureCase {
    param([string]$Filter)
    Assert-NativeSelection ($Filter.StartsWith('=')) 'production component calls must use exact selection'
    $Name = $Filter.Substring(1)
    $script:NativeFixtureCalls.Add("case:$Name")
    $global:LASTEXITCODE = if ($script:NativeFixtureMode -eq 'failure') { 1 } else { 0 }
    if ($script:NativeFixtureMode -eq 'wrong-name') { '[RUN ] unexpected_case' } else { "[RUN ] $Name" }
    if ($script:NativeFixtureMode -eq 'wrong-count') { '[RUN ] unexpected_second_case'; '2 tests, 0 failed' }
    elseif ($script:NativeFixtureMode -ne 'no-summary') { '1 tests, 0 failed' }
}

$controlled = [pscustomobject]@{ mode = 'component'; components = @('fixture'); tests = @('first', 'second'); requireHip = $true }
Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli | Out-Null
Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first|case:second|cli:gpu-info') 'each case once with before/after HIP checks'
foreach ($mode in @('failure', 'wrong-name', 'wrong-count', 'no-summary')) {
    $script:NativeFixtureCalls.Clear()
    $script:NativeFixtureMode = $mode
    Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli } 'did not run exactly once and pass'
    Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first') "stop after first failed case: $mode"
}
$script:NativeFixtureMode = 'pass'
foreach ($mode in @('not-compiled', 'unavailable', 'lost')) {
    $script:NativeFixtureCalls.Clear()
    $script:NativeFixtureHipMode = $mode
    Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli } 'requires a compiled HIP backend'
    $expectedCalls = if ($mode -eq 'lost') { 'cli:gpu-info|case:first|case:second|cli:gpu-info' } else { 'cli:gpu-info' }
    Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq $expectedCalls) "actual HIP readiness rejection: $mode"
}
$script:NativeFixtureHipMode = 'ready'
Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -CpuOnlyValidation } 'CPU-only binary'
$script:NativeFixtureHipMode = 'cpu-only'
$script:NativeFixtureCalls.Clear()
$cpuOutput = @(Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -CpuOnlyValidation)
Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first|case:second|cli:gpu-info') 'explicit CPU validation retains exact cases and backend checks'
Assert-NativeSelection (@($cpuOutput | Where-Object { $_ -match 'GPU assertions are not qualified' }).Count -eq 1) 'CPU results must not imply GPU qualification'
foreach ($mode in @('cpu-success', 'cpu-error')) {
    $script:NativeFixtureHipMode = $mode
    $script:NativeFixtureCalls.Clear()
    Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -CpuOnlyValidation } 'CPU-only binary'
    Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info') "CPU info exit status must match the CLI unavailable contract: $mode"
}
$script:NativeFixtureHipMode = 'cpu-only'
Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli } 'requires a compiled HIP backend'
Assert-FlitzZipCpuValidationReceipt -Receipt ([pscustomobject]@{ recipe = @{ FLITZZIP_ENABLE_HIP = 'OFF' } })
Assert-NativeSelectionRejected { Assert-FlitzZipCpuValidationReceipt -Receipt ([pscustomobject]@{ recipe = @{ FLITZZIP_ENABLE_HIP = 'ON' } }) } 'HIP OFF'
$script:NativeFixtureHipMode = 'ready'
foreach ($cases in @(@(), @('first', 'first'))) {
    $invalid = [pscustomobject]@{ mode = 'component'; tests = $cases; requireHip = $true }
    $script:NativeFixtureCalls.Clear()
    Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $invalid -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli } 'nonempty unique selection'
    Assert-NativeSelection ($script:NativeFixtureCalls.Count -eq 0) 'invalid plans must execute nothing'
}
Write-Output 'Native selection registry, component, HIP, exact execution and early-stop contracts passed.'
