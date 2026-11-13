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

# Follow local test-header dependencies independently of the production selector's
# component rules. Reads stay within the canonical test tree, reject linked or
# oversized inputs, and terminate on include cycles; no native code is executed.
function Get-NativeTestHeaderClosure {
    param([string]$Root, [string]$Source, [hashtable]$TextCache)
    $testPrefix = [IO.Path]::GetFullPath((Join-Path $Root 'tests/cpp')) + [IO.Path]::DirectorySeparatorChar
    $first = [IO.Path]::GetFullPath((Join-Path $Root $Source))
    Assert-NativeSelection ($first.StartsWith($testPrefix, [StringComparison]::OrdinalIgnoreCase)) 'header consumers require canonical registered test paths'
    $pending = [Collections.Generic.Stack[string]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pending.Push($first)
    while ($pending.Count) {
        $current = $pending.Pop()
        if (-not $seen.Add($current)) { continue }
        Assert-NativeSelection ($seen.Count -le 1024) 'local test-header dependency bound'
        if (-not $TextCache.ContainsKey($current)) {
            $item = Get-Item -LiteralPath $current -ErrorAction Stop
            Assert-NativeSelection (-not $item.PSIsContainer -and
                -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
                $item.Length -le 1MB) 'test-header evidence must be a bounded regular source file'
            $repoPrefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
            $ancestor = $item.Directory
            while ($ancestor -and $ancestor.FullName.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                Assert-NativeSelection (-not ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'test-header ancestors must stay in the owned source tree'
                $ancestor = $ancestor.Parent
            }
            $TextCache[$current] = [IO.File]::ReadAllText($current, [Text.Encoding]::UTF8)
        }
        foreach ($match in [regex]::Matches($TextCache[$current], '(?m)^[^\S\r\n]*#[^\S\r\n]*include[^\S\r\n]*"([^"\r\n]+)"')) {
            $candidate = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $current) $match.Groups[1].Value))
            if ($candidate.StartsWith($testPrefix, [StringComparison]::OrdinalIgnoreCase) -and
                [IO.Path]::GetExtension($candidate) -in @('.h', '.hpp') -and
                (Test-Path -LiteralPath $candidate -PathType Leaf)) {
                $pending.Push($candidate)
            }
        }
    }
    return ,$seen
}

# An independently discovered direct or transitive header consumer must retain
# every registered case in its translation unit and its actual HIP admission.
# Broad selection is safe; this guard catches incomplete component mappings.
function Assert-NativeHeaderConsumersSelected {
    param([string]$Root, [string]$Header, [object]$Selection, [object[]]$Inventory, [hashtable]$TextCache)
    if ($Selection.mode -ceq 'full') { return }
    $headerPath = [IO.Path]::GetFullPath((Join-Path $Root $Header))
    foreach ($group in @($Inventory | Group-Object source)) {
        $closure = Get-NativeTestHeaderClosure -Root $Root -Source $group.Name -TextCache $TextCache
        if (-not $closure.Contains($headerPath)) { continue }
        foreach ($case in $group.Group) {
            Assert-NativeSelection ($case.name -cin $Selection.tests) "mapped header missing registered consumer: $($case.name)"
        }
        $sourcePath = [IO.Path]::GetFullPath((Join-Path $Root $group.Name))
        if ($TextCache[$sourcePath] -match '(?m)^[^\S\r\n]*#[^\S\r\n]*(?:if|ifdef|ifndef|elif)\b[^\r\n]*\bFLITZZIP_ENABLE_HIP\b') {
            Assert-NativeSelection $Selection.requireHip "mapped header dropped HIP admission: $($group.Name)"
        }
    }
}

$entropyPaths = @('src/gpu/hip_codec_static_prefix.hip.cpp', 'src/gpu/hip_codec_adaptive_prefix.hip.cpp')
$ransExpected = @('gpu_rans_reader_independent_payloads_and_segment_resets',
    'gpu_rans_reader_shared_singleton_fixture_matches_independent_wire',
    'gpu_rans_reader_full_width_singleton_and_complete_sparse_cdf',
    'gpu_rans_reader_all_options_preserve_exact_singleton_state',
    'gpu_rans_reader_maximum_context_inventory_has_bounded_caller_scratch',
    'gpu_rans_reader_non_singleton_maximum_u16_frequency_is_full_width',
    'gpu_rans_reader_rejects_every_truncated_prefix_and_invalid_extent',
    'gpu_rans_reader_rejects_noncanonical_models_and_offsets',
    'gpu_rans_reader_rejects_coded_state_and_missing_context_without_fallback',
    'gpu_rans_reader_rejects_truncated_renormalization_with_valid_framing',
    'gpu_rans_reader_admits_capacity_before_mutation_and_preserves_guards',
    'gpu_rans_reader_rejects_live_aliases_but_allows_unused_scratch_reuse')
$ransDeviceExpected = @('gpu_rans_kernel_independent_frames_and_full_width_models',
    'gpu_rans_kernel_all_options_and_maximum_segment_work',
    'gpu_rans_kernel_rejects_uncontained_and_unbounded_plans',
    'gpu_rans_kernel_rejects_model_state_and_stream_faults_then_recovers',
    'gpu_rans_operation_workspace_contract_is_exact_and_bounded',
    'gpu_rans_operation_independent_bytes_device_crc_and_dispatch_boundaries',
    'gpu_rans_operation_checkpoints_see_completed_windows_and_recover',
    'gpu_rans_operation_rejects_later_corruption_without_publishing_or_retaining_scratch',
    'gpu_rans_operation_rejects_extent_alias_and_scratch_errors_before_device_work')
foreach ($source in @('src/core/rans_block.cpp', 'src/core/rans_block.hpp', 'tests/cpp/rans_fixtures.hpp',
        'src/core/byte_region.hpp')) {
    $rans = Get-FlitzZipNativeTestSelection -Paths @($source)
    $expected = @($ransExpected + $ransDeviceExpected)
    if ($source -ceq 'src/core/byte_region.hpp') {
        $expected += 'byte_region_alias_admission_matches_independent_byte_membership'
    }
    Assert-NativeSelection ($rans.mode -ceq 'component' -and $rans.requireHip -and
        'rans-reader' -in $rans.components -and 'rans-device' -in $rans.components -and
        (($rans.tests | Sort-Object) -join '|') -ceq (($expected | Sort-Object) -join '|')) 'shared reader and fixture changes retain every CPU and required-HIP consumer'
}
foreach ($source in @('src/gpu/hip_rans.cpp', 'src/gpu/hip_rans.hpp',
        'src/gpu/hip_rans.hip.cpp', 'src/gpu/rans_decode_plan.hpp')) {
    $rans = Get-FlitzZipNativeTestSelection -Paths @($source)
    Assert-NativeSelection ($rans.mode -ceq 'component' -and $rans.requireHip -and
        (($rans.tests | Sort-Object) -join '|') -ceq (($ransDeviceExpected | Sort-Object) -join '|')) 'device reader inputs retain exact real-HIP kernel and operation consumers'
}
$ransBroad = Get-FlitzZipNativeTestSelection -Paths @('src/core/rans_block.cpp', 'CMakeLists.txt')
Assert-NativeSelection ($ransBroad.mode -ceq 'full') 'new reader mapping must not narrow CMake qualification'
$countsExpected = @('gpu_rans_counts_match_raw_adjacency_and_every_segment_context',
    'gpu_rans_counts_zero_predecessors_and_disappearing_contexts_reject_omitted_work',
    'gpu_rans_counts_projection_rejects_underflow_and_mass_overflow_then_recovers',
    'gpu_rans_counts_maximum_source_stays_bounded_and_preserves_all_bytes',
    'gpu_rans_counts_invalid_ranges_preserve_tables_and_inactive_guards',
    'gpu_rans_counts_operation_reuses_complete_statistics_across_all_projections',
    'gpu_rans_counts_operation_cancellation_invalidates_partial_caches_and_recovers',
    'gpu_rans_counts_operation_rejects_options_without_poisoning_completed_cache',
    'gpu_rans_counts_operation_maximum_source_and_independent_lifetimes')
foreach ($source in @('src/gpu/hip_rans_counts.hip.cpp', 'src/gpu/rans_count_plan.hpp',
        'src/gpu/hip_rans_statistics.cpp', 'src/gpu/hip_rans_statistics.hpp',
        'tests/cpp/test_gpu_rans_counts.cpp')) {
    $counts = Get-FlitzZipNativeTestSelection -Paths @($source)
    Assert-NativeSelection ($counts.mode -ceq 'component' -and $counts.requireHip -and
        (($counts.tests | Sort-Object) -join '|') -ceq (($countsExpected | Sort-Object) -join '|')) 'count inputs retain every exact raw-statistics and fault consumer'
}
$modelsExpected = @('gpu_rans_models_row_mass_admission_rejects_partial_and_forged_metadata',
    'gpu_rans_models_integer_quotas_match_independent_golden_vectors',
    'gpu_rans_models_missing_contexts_and_full_width_singletons_are_explicit',
    'gpu_rans_models_corrupted_order_mass_and_frequencies_reject_before_publication',
    'gpu_rans_models_invalid_launch_and_precision_leave_guarded_storage_unchanged',
    'gpu_rans_streams_singleton_states_match_independent_fixed_bytes',
    'gpu_rans_streams_device_counts_models_and_bytes_cover_every_admitted_option',
    'gpu_rans_streams_invalid_plans_and_corrupted_models_reject_then_recover',
    'gpu_rans_streams_compaction_rejects_corrupt_ranges_before_dense_publication',
    'gpu_rans_streams_maximum_source_bound_and_multiple_dispatches')
foreach ($source in @('src/gpu/rans_model_plan.hpp', 'src/gpu/hip_rans_models.hip.cpp',
        'src/gpu/hip_rans_encode.hip.cpp', 'tests/cpp/rans_model_fixtures.hpp', 'tests/cpp/rans_stream_fixtures.hpp')) {
    $models = Get-FlitzZipNativeTestSelection -Paths @($source)
    Assert-NativeSelection ($models.mode -ceq 'component' -and $models.requireHip -and
        (($models.tests | Sort-Object) -join '|') -ceq (($modelsExpected | Sort-Object) -join '|')) 'model and stream inputs retain every exact producer, wire-reader and failure consumer'
}
$sharedRansFixture = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/rans_gpu_fixtures.hpp')
Assert-NativeSelection ($sharedRansFixture.requireHip -and
    (($sharedRansFixture.tests | Sort-Object) -join '|') -ceq ((@($countsExpected + $modelsExpected) | Sort-Object) -join '|')) 'shared guarded GPU buffers retain all statistics, model and stream consumers'
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
$sparseExpected = @(
    'flzip_gpu_efforts_preserve_dictionary_candidates_after_entropy_gain',
    'flzip_gpu_entropy_efforts_preserve_per_block_winners',
    'flzip_long_sparse_pattern_v7_reader_roundtrip', 'flzip_long_sparse_pattern_writer_selects_v7',
    'flzip_sparse_pattern_cpu_reader_roundtrip', 'long_sparse_pattern_required_hip_roundtrip',
    'sparse_pattern_block_admits_canonical_payload', 'sparse_pattern_block_rejects_malformed_payload',
    'sparse_pattern_candidate_layout_precedes_device_work',
    'sparse_pattern_field_reader_rejects_truncation_and_wrapping_offsets',
    'sparse_pattern_hip_batch_preserves_block_offsets',
    'sparse_pattern_hip_batch_rejects_prepopulated_output_fields', 'sparse_pattern_long_period_boundaries',
    'sparse_pattern_replacements_validate_complete_internal_results', 'sparse_pattern_required_hip_encode_roundtrip'
)
Assert-NativeSelection ($sparse.mode -eq 'component' -and $sparse.requireHip -and
    (($sparse.tests | Sort-Object) -join '|') -ceq (($sparseExpected | Sort-Object) -join '|')) 'exact reviewed sparse candidate cohort and required HIP'
foreach ($name in @('sparse_pattern_hip_batch_rejects_prepopulated_output_fields',
        'sparse_pattern_hip_batch_preserves_block_offsets', 'long_sparse_pattern_required_hip_roundtrip',
        'flzip_sparse_pattern_cpu_reader_roundtrip', 'flzip_long_sparse_pattern_v7_reader_roundtrip',
        'flzip_long_sparse_pattern_writer_selects_v7',
        'sparse_pattern_field_reader_rejects_truncation_and_wrapping_offsets')) {
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
$selectorText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'native_test_selection.ps1'), [Text.Encoding]::UTF8)
$mappedHeaders = @([regex]::Matches($selectorText, '(?i)tests/cpp/[A-Za-z0-9_./-]+\.h(?:pp)?\b') |
    ForEach-Object Value | Sort-Object -Unique)
$headerTextCache = @{}
foreach ($header in $mappedHeaders) {
    $selection = Get-FlitzZipNativeTestSelection -Paths @($header)
    Assert-NativeHeaderConsumersSelected -Root $root -Header $header -Selection $selection -Inventory $inventory -TextCache $headerTextCache
}
$ctestFixture = @'
{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[
{"name":"flitzzip_tests","properties":[{"name":"LABELS","value":["mixed-native"]}]},
{"name":"contracts","properties":[{"name":"LABELS","value":["correctness","dependency"]}]},
{"name":"measured","properties":[{"name":"LABELS","value":["measurement"]}]}]}
'@
Assert-FlitzZipCTestExecutionInventory -Inventory ($ctestFixture | ConvertFrom-Json)
foreach ($invalid in @(
        $ctestFixture.Replace('"major":1', '"major":2'),
        $ctestFixture.Replace('"correctness","dependency"', '"unknown"'),
        $ctestFixture.Replace('"correctness","dependency"', '"correctness","measurement"'),
        $ctestFixture.Replace('["correctness","dependency"]', '"correctness"'),
        $ctestFixture.Replace('"name":"contracts"', '"name":"flitzzip_tests"'),
        $ctestFixture.Replace('"mixed-native"', '"correctness"'),
        $ctestFixture.Replace('"name":"flitzzip_tests"', '"name":"unknown_mixed_registry"'),
        $ctestFixture.Replace('"name":"contracts"', '"name":42'),
        $ctestFixture.Replace('"correctness","dependency"', '"measurement"'),
        '{"kind":"ctestInfo","version":{"major":1},"tests":[]}')) {
    Assert-NativeSelectionRejected { Assert-FlitzZipCTestExecutionInventory -Inventory ($invalid | ConvertFrom-Json) } 'CTest'
}
$measurementExpected = @('memory_benchmark_source_handoff_rejects_stale_metadata_and_recovers',
    'memory_benchmark_cpu_pipeline_is_byte_exact_and_ram_only',
    'memory_benchmark_all_efforts_preserve_corpus_bytes_and_backend_identity',
    'memory_benchmark_neutron_pipeline_requires_real_hip_and_exact_bytes',
    'memory_benchmark_suite_accounts_for_container_overhead')
$measurementActual = @($inventory | Where-Object role -eq 'Measurement' | Select-Object -ExpandProperty name)
Assert-NativeSelection ((($measurementActual | Sort-Object) -join '|') -ceq
    (($measurementExpected | Sort-Object) -join '|')) 'live benchmark pipelines require explicit measurement roles; other correctness cases remain admitted'
$folderCachePath = 'src/app/folder_size_cache.hpp'
$folderCacheExpected = @($inventory | Where-Object source -eq 'tests/cpp/test_folder_size_cache.cpp' |
    Select-Object -ExpandProperty name | Sort-Object)
$folderCache = Get-FlitzZipNativeTestSelection -Paths @($folderCachePath)
Assert-NativeSelection ($folderCache.mode -eq 'component' -and -not $folderCache.requireHip -and
    ($folderCache.tests -join '|') -ceq ($folderCacheExpected -join '|')) 'cache selects every registered lifetime and allocation-failure case'
$folderCacheBatch = Get-FlitzZipNativeTestSelection -Paths @($folderCachePath, $folderCachePath,
    'tests/cpp/test_folder_size_cache.cpp')
Assert-NativeSelection (($folderCacheBatch.tests -join '|') -ceq ($folderCacheExpected -join '|')) 'cache changes and tests form one complete deduplicated cohort'
$folderCacheGpu = Get-FlitzZipNativeTestSelection -Paths (@($folderCachePath) + $entropyPaths)
$folderCacheGpuExpected = @($folderCacheExpected + $entropyExpected | Sort-Object -Unique)
Assert-NativeSelection ($folderCacheGpu.requireHip -and
    ($folderCacheGpu.tests -join '|') -ceq ($folderCacheGpuExpected -join '|')) 'cache selection preserves unrelated required-HIP consumers'
foreach ($shared in @('src/app/main_window.hpp', 'src/app/queue_metadata.hpp', 'src/app/unreviewed_cache.hpp',
        'CMakeLists.txt', 'tests/cpp/test_util.hpp')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths @($folderCachePath, $shared)).mode -eq 'full') "shared or unreviewed cache consumers retain full native qualification: $shared"
}
Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths @($folderCachePath) -Full).mode -eq 'full') 'explicit full cache qualification remains broad'
$monitorPaths = @('src/app/cpu_sampling.hpp', 'src/app/pdh_percentage_sampler.hpp',
    'src/app/performance_counters.hpp', 'src/app/graph_series.hpp', 'src/app/main_window_monitor.cpp')
$monitorExpected = @($inventory | Where-Object { $_.source -in @('tests/cpp/test_system_layout.cpp',
        'tests/cpp/test_resource_usage.cpp') } | Select-Object -ExpandProperty name | Sort-Object)
foreach ($monitorPath in $monitorPaths) {
    $monitor = Get-FlitzZipNativeTestSelection -Paths @($monitorPath)
    Assert-NativeSelection ($monitor.mode -eq 'component' -and -not $monitor.requireHip -and
        ($monitor.tests -join '|') -ceq ($monitorExpected -join '|')) "complete monitor and accounting consumers: $monitorPath"
}
$monitorBatch = Get-FlitzZipNativeTestSelection -Paths ($monitorPaths + $monitorPaths + 'tests/cpp/test_system_layout.cpp')
Assert-NativeSelection (($monitorBatch.tests -join '|') -ceq ($monitorExpected -join '|')) 'monitor batch retains every case without repetition'
$monitorGpu = Get-FlitzZipNativeTestSelection -Paths ($monitorPaths + $entropyPaths)
$monitorGpuExpected = @($monitorExpected + $entropyExpected | Sort-Object -Unique)
Assert-NativeSelection ($monitorGpu.requireHip -and
    ($monitorGpu.tests -join '|') -ceq ($monitorGpuExpected -join '|')) 'monitor selection cannot discard GPU consumers or hardware admission'
foreach ($shared in @('src/app/main_window.hpp', 'src/app/main_window_state.hpp', 'src/app/unreviewed_monitor.hpp',
        'src/core/resource_usage.cpp', 'CMakeLists.txt', 'tests/cpp/test_util.hpp')) {
    Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths ($monitorPaths + $shared)).mode -eq 'full') "unreviewed/shared monitor dependencies retain broad coverage: $shared"
}
Assert-NativeSelection ((Get-FlitzZipNativeTestSelection -Paths $monitorPaths -Full).mode -eq 'full') 'explicit full monitor qualification remains broad'
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
$guiPaths = @('tests/cpp/test_main_window_extraction.cpp', 'tests/cpp/test_main_window_queue_controls.cpp')
$guiExpected = @($inventory | Where-Object { $_.source -in $guiPaths } | Select-Object -ExpandProperty name | Sort-Object)
$guiSelection = Get-FlitzZipNativeTestSelection -Paths $guiPaths
Assert-NativeSelection ($guiExpected.Count -gt 0 -and $guiSelection.mode -eq 'component' -and
    -not $guiSelection.requireHip -and ($guiSelection.tests -join '|') -ceq ($guiExpected -join '|')) 'conditional GUI target sources select every registered changed case'
$winSource = 'tests/cpp/test_compression_stream_io.cpp'
$winExpected = @($inventory | Where-Object source -eq $winSource | Select-Object -ExpandProperty name | Sort-Object)
$winSelection = Get-FlitzZipNativeTestSelection -Paths @($winSource)
Assert-NativeSelection ($winExpected.Count -gt 0 -and $winSelection.mode -eq 'component' -and
    ($winSelection.tests -join '|') -ceq ($winExpected -join '|')) 'conditional Windows test source remains inventoried'

$ownedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/native-selection-tests'))
$fixtureRoot = Join-Path $ownedRoot ([guid]::NewGuid().ToString('N'))
$fixtureTests = Join-Path $fixtureRoot 'tests/cpp'
New-Item -ItemType Directory -Path $fixtureTests -Force | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)')
    $fixtureSource = Join-Path $fixtureTests 'test_fixture.cpp'
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(sample_longer) {}")
    $sharedHeader = Join-Path $fixtureTests 'shared.hpp'
    $nestedHeader = Join-Path $fixtureTests 'nested.HPP'
    [IO.File]::WriteAllText($sharedHeader, "#pragma once`n#include `"nested.HPP`"")
    [IO.File]::WriteAllText($nestedHeader, "#pragma once`n#include `"shared.hpp`"")
    $outsideHeader = Join-Path (Split-Path -Parent $fixtureTests) 'outside.hpp'
    [IO.File]::WriteAllText($outsideHeader, '#pragma once')
    [IO.File]::WriteAllText($fixtureSource, "#include `"nested.HPP`"`n#include `"../outside.hpp`"`nTEST_CASE(sample) {}`nTEST_CASE(sample_longer) {}")
    $outsideClosure = Get-NativeTestHeaderClosure -Root $fixtureRoot -Source 'tests/cpp/test_fixture.cpp' -TextCache @{}
    Assert-NativeSelection (-not $outsideClosure.Contains([IO.Path]::GetFullPath($outsideHeader))) 'test-header traversal must not read outside its declared scope'
    $lateSource = Join-Path $fixtureTests 'test_late.cpp'
    [IO.File]::WriteAllText($lateSource, @'
#include "nested.HPP"
TEST_CASE(late_consumer) {
#if FLITZZIP_ENABLE_HIP
    auto info = query_gpu_info();
#endif
}
'@)
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp tests/cpp/test_late.cpp)')
    $lateInventory = @(Get-FlitzZipNativeTestInventory -Root $fixtureRoot)
    $incomplete = [pscustomobject]@{ mode = 'component'; tests = @('sample', 'sample_longer'); requireHip = $true }
    $lateCache = @{}
    Assert-NativeSelectionRejected {
        Assert-NativeHeaderConsumersSelected -Root $fixtureRoot -Header 'tests/cpp/shared.hpp' -Selection $incomplete -Inventory $lateInventory -TextCache $lateCache
    } 'missing registered consumer: late_consumer'
    $incomplete.tests += 'late_consumer'
    $incomplete.requireHip = $false
    Assert-NativeSelectionRejected {
        Assert-NativeHeaderConsumersSelected -Root $fixtureRoot -Header 'tests/cpp/shared.hpp' -Selection $incomplete -Inventory $lateInventory -TextCache $lateCache
    } 'dropped HIP admission'
    $incomplete.requireHip = $true
    Assert-NativeHeaderConsumersSelected -Root $fixtureRoot -Header 'tests/cpp/shared.hpp' -Selection $incomplete -Inventory $lateInventory -TextCache $lateCache
    Assert-NativeHeaderConsumersSelected -Root $fixtureRoot -Header 'tests/cpp/shared.hpp' -Selection ([pscustomobject]@{ mode = 'full' }) -Inventory $lateInventory -TextCache $lateCache
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)')
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(sample_longer) {}")
    $conditionalGui = Join-Path $fixtureTests 'test_gui.cpp'
    [IO.File]::WriteAllText($conditionalGui, 'TEST_CASE(gui_click) {}')
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), @'
add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)
if(TARGET flitzzip_gui)
  target_sources(flitzzip_test_objects PRIVATE tests/cpp/test_gui.cpp)
endif()
'@)
    $conditionalSelection = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_gui.cpp') -Root $fixtureRoot
    Assert-NativeSelection ($conditionalSelection.mode -eq 'component' -and
        ($conditionalSelection.tests -join '|') -ceq 'gui_click') 'conditional GUI target source is selected without a full driver'
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), @'
add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)
target_sources(flitzzip_test_objects PUBLIC tests/cpp/test_gui.cpp)
'@)
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'Unsupported native test object target_sources'
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(flitzzip_test_objects OBJECT tests/cpp/test_fixture.cpp)')
    $overlapping = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_fixture.cpp') -Root $fixtureRoot
    Assert-NativeSelection ($overlapping.tests.Count -eq 2) 'overlapping names remain distinct exact cases'
    Assert-NativeSelection (-not $overlapping.requireHip) 'ordinary registered sources do not require a device'
    foreach ($deviceBody in @("#if FLITZZIP_ENABLE_HIP`n// device assertions`n#endif",
            "#if 0`n// inactive branch`n#elif FLITZZIP_ENABLE_HIP`n// device assertions`n#endif",
            'if (!flitzzip::query_gpu_info().available) { return; }',
            'if (!flitzzip::hip_device_available()) { return; }',
            'flitzzip::require_hip_device_ready();',
            'const auto& kernels = flitzzip::hip_kernel_api();')) {
        [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(new_device_boundary) {`n$deviceBody`n}")
        $deviceSelection = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_fixture.cpp') -Root $fixtureRoot
        Assert-NativeSelection ($deviceSelection.mode -eq 'component' -and $deviceSelection.requireHip -and
            ($deviceSelection.tests -join '|') -ceq 'new_device_boundary') 'new device suites cannot pass by skipping HIP'
    }
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
    [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(measured, TestExecutionRole::Measurement) {}")
    $roleInventory = @(Get-FlitzZipNativeTestInventory -Root $fixtureRoot)
    Assert-NativeSelection (($roleInventory.name -join '|') -ceq 'sample|measured' -and
        ($roleInventory.role -join '|') -ceq 'Correctness|Measurement') 'both execution roles retain inventory and exact selection'
    $roleSelection = Get-FlitzZipNativeTestSelection -Paths @('tests/cpp/test_fixture.cpp') -Root $fixtureRoot
    Assert-NativeSelection (($roleSelection.tests -join '|') -ceq 'measured|sample') 'default selection does not lose measurement cases'
    foreach ($role in @('Unknown', 'static_cast<TestExecutionRole>(42)')) {
        [IO.File]::WriteAllText($fixtureSource, "TEST_CASE(sample) {}`nTEST_CASE(measured, $role) {}")
        Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'unsupported registration syntax'
    }
    [IO.File]::WriteAllText($fixtureSource, '// no registrations')
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'no recognized cases'
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), 'add_library(different_target OBJECT test.cpp)')
    Assert-NativeSelectionRejected { Get-FlitzZipNativeTestInventory -Root $fixtureRoot } 'Cannot resolve'
    [IO.File]::WriteAllText((Join-Path $fixtureRoot 'CMakeLists.txt'), @'
add_library(flitzzip_test_objects OBJECT
  tests/cpp/test_sevenzip_compat.cpp
  tests/cpp/test_sevenzip_ppmd.cpp
  tests/cpp/test_fixture.cpp)
'@)
    [IO.File]::WriteAllText((Join-Path $fixtureTests 'test_sevenzip_compat.cpp'),
        "TEST_CASE(sevenzip_input_bounds) {}`nTEST_CASE(shared_seek_bounds) {}")
    [IO.File]::WriteAllText((Join-Path $fixtureTests 'test_sevenzip_ppmd.cpp'), 'TEST_CASE(decoder_owner_cleanup) {}')
    [IO.File]::WriteAllText($fixtureSource, 'TEST_CASE(unrelated_codec) {}')
    $ownedSevenzip = Get-FlitzZipNativeTestSelection -Paths @('third_party/lzma_sdk/C/7zArcIn.c') -Root $fixtureRoot
    $ownedExpected = @('decoder_owner_cleanup', 'sevenzip_input_bounds', 'shared_seek_bounds')
    Assert-NativeSelection ($ownedSevenzip.mode -eq 'component' -and -not $ownedSevenzip.requireHip -and
        ($ownedSevenzip.tests -join '|') -ceq ($ownedExpected -join '|')) 'format-owned parser tests are complete regardless of names and exclude unrelated sources'
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
    $assertionFailed = $script:NativeFixtureMode -eq 'assertion-failure' -or
        ($script:NativeFixtureMode -eq 'mixed-failure' -and $Name -eq 'first')
    $global:LASTEXITCODE = if ($script:NativeFixtureMode -eq 'crash') { -1073741819 }
        elseif ($script:NativeFixtureMode -eq 'failure' -or $assertionFailed) { 1 } else { 0 }
    if ($script:NativeFixtureMode -eq 'wrong-name') { '[RUN ] unexpected_case' } else { "[RUN ] $Name" }
    if ($script:NativeFixtureMode -eq 'wrong-count') { '[RUN ] unexpected_second_case'; '2 tests, 0 failed' }
    elseif ($script:NativeFixtureMode -eq 'conflicting-summary') { '1 tests, 1 failed'; '1 tests, 0 failed' }
    elseif ($assertionFailed) { '1 tests, 1 failed' }
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
$script:NativeFixtureMode = 'assertion-failure'
$script:NativeFixtureCalls.Clear()
Assert-NativeSelectionRejected { Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli } 'did not run exactly once and pass'
Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first') 'default execution still stops on an ordinary assertion failure'
foreach ($mode in @('assertion-failure', 'mixed-failure')) {
    $script:NativeFixtureMode = $mode
    $script:NativeFixtureCalls.Clear()
    $expectedFailures = if ($mode -eq 'assertion-failure') { 'first, second' } else { 'first' }
    Assert-NativeSelectionRejected {
        Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -ContinueAfterAssertionFailure
    } "Native batch assertion failures: $expectedFailures"
    Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first|cli:gpu-info|case:second|cli:gpu-info') 'batch executes each case once and retains readiness after failures'
}
foreach ($mode in @('failure', 'wrong-name', 'wrong-count', 'no-summary', 'crash', 'conflicting-summary')) {
    $script:NativeFixtureMode = $mode
    $script:NativeFixtureCalls.Clear()
    Assert-NativeSelectionRejected {
        Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -ContinueAfterAssertionFailure
    } 'did not run exactly once and pass'
    Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first') "batch stops on process/registry failure: $mode"
}
$script:NativeFixtureMode = 'assertion-failure'
$script:NativeFixtureHipMode = 'lost'
$script:NativeFixtureCalls.Clear()
Assert-NativeSelectionRejected {
    Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -ContinueAfterAssertionFailure
} 'requires a compiled HIP backend'
Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first|cli:gpu-info') 'batch must stop if an assertion failure coincides with lost HIP readiness'
$script:NativeFixtureHipMode = 'ready'
$script:NativeFixtureMode = 'pass'
$script:NativeFixtureCalls.Clear()
$batchOutput = @(Invoke-FlitzZipNativeTestSelection -Selection $controlled -TestRunner Invoke-NativeFixtureCase -Cli Invoke-NativeFixtureCli -ContinueAfterAssertionFailure)
Assert-NativeSelection (($script:NativeFixtureCalls -join '|') -eq 'cli:gpu-info|case:first|case:second|cli:gpu-info') 'passing batch does not repeat cases or readiness checks'
Assert-NativeSelection (@($batchOutput | Where-Object { $_ -match '^Native component tests passed: 2 cases;' }).Count -eq 1) 'passing batch emits exactly one truthful summary'
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
# Purpose: Exercise the actual absence gate with independent status/field combinations and CLI acquisition failure.
# Inputs: One fixture row and the expected dependency-check command; caller preferences must remain unchanged.
# Outputs: Returns declared public capability fields or throws; never invokes a native workload.
function Invoke-NativeAbsenceFixtureCli {
    param([string]$Argument)
    Assert-NativeSelection ($Argument -ceq 'dependency-check') 'absence gate uses dependency admission'
    Assert-NativeSelection (-not $PSNativeCommandUseErrorActionPreference) 'expected absence codes are captured'
    if ($script:NativeAbsenceCase.ContainsKey('throw')) { throw 'independent CLI acquisition failure' }
    $global:LASTEXITCODE = $script:NativeAbsenceCase.exitCode
    if ($script:NativeAbsenceCase.ContainsKey('compiled')) { "hip_compiled=$($script:NativeAbsenceCase.compiled)" }
    if ($script:NativeAbsenceCase.ContainsKey('available')) { "available=$($script:NativeAbsenceCase.available)" }
}

$absenceCases = @(
    @{ exitCode = 11; compiled = 'true'; available = 'false'; accepted = $true },
    @{ exitCode = 12; compiled = 'true'; available = 'false'; accepted = $true },
    @{ exitCode = 0; compiled = 'true'; available = 'true'; accepted = $false },
    @{ exitCode = 13; compiled = 'true'; available = 'false'; accepted = $false },
    @{ exitCode = 1; compiled = 'true'; available = 'false'; accepted = $false },
    @{ exitCode = 10; compiled = 'false'; available = 'false'; accepted = $false },
    @{ exitCode = 11; compiled = 'false'; available = 'false'; accepted = $false },
    @{ exitCode = 12; compiled = 'true'; available = 'true'; accepted = $false },
    @{ exitCode = 11; available = 'false'; accepted = $false },
    @{ exitCode = 12; compiled = 'true'; accepted = $false }
)
$priorNativePreference = Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue
$priorNativePreferenceValue = if ($null -ne $priorNativePreference) { $priorNativePreference.Value } else { $null }
try {
    foreach ($parentPreference in @($false, $true)) {
        $PSNativeCommandUseErrorActionPreference = $parentPreference
        foreach ($case in $absenceCases) {
            $script:NativeAbsenceCase = $case
            if ($case.accepted) {
                Assert-FlitzZipNativeHipAbsence -Cli Invoke-NativeAbsenceFixtureCli | Out-Null
                Assert-NativeSelection ($LASTEXITCODE -eq 0) 'admitted absence normalizes its handled status'
            } else {
                Assert-NativeSelectionRejected {
                    Assert-FlitzZipNativeHipAbsence -Cli Invoke-NativeAbsenceFixtureCli
                } 'admitted runtime/device absence'
            }
            Assert-NativeSelection ($PSNativeCommandUseErrorActionPreference -eq $parentPreference) 'absence gate restores caller preferences'
        }
        $script:NativeAbsenceCase = @{ throw = $true }
        Assert-NativeSelectionRejected {
            Assert-FlitzZipNativeHipAbsence -Cli Invoke-NativeAbsenceFixtureCli
        } 'independent CLI acquisition failure'
        Assert-NativeSelection ($PSNativeCommandUseErrorActionPreference -eq $parentPreference) 'failed absence query restores caller preferences'
    }
} finally {
    if ($null -ne $priorNativePreference) {
        $PSNativeCommandUseErrorActionPreference = $priorNativePreferenceValue
    } else {
        Remove-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue
    }
}
$global:LASTEXITCODE = 0
Write-Output 'Native selection registry, component, HIP, absence, exact execution and early-stop contracts passed.'
