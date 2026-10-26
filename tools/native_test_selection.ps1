# Purpose: Decode canonical changed-path metadata consistently across Windows PowerShell and PowerShell 7.
# Inputs: Base64 contains a UTF-8 JSON array of normalized checkout-relative path strings.
# Outputs: Returns path strings; rejects malformed encoding, non-array JSON and unsafe or empty paths.
function ConvertFrom-FlitzZipNativeSelectionPath {
    param([Parameter(Mandatory = $true)][string]$Base64)
    $text = [Text.UTF8Encoding]::new($false, $true).GetString([Convert]::FromBase64String($Base64))
    if (-not $text.TrimStart().StartsWith('[')) { throw 'Native selection requires a JSON path array.' }
    $parsed = ConvertFrom-Json -InputObject $text
    $paths = @($parsed)
    if (-not $paths.Count -or @($paths | Where-Object { $_ -isnot [string] -or
                [string]::IsNullOrWhiteSpace($_) -or $_ -match '(^/|\\|(^|/)\.\.(/|$)|:)' }).Count) {
        throw 'Native selection requires normalized checkout-relative changed paths.'
    }
    return $paths
}

# Purpose: Prevent source inventories from selecting registrations absent in a supported Windows build.
# Inputs: Text and Source identify a registered test source; only positive Windows platform guards are known.
# Outputs: Rejects conditional registrations with unknown configuration truth; conditions inside test bodies remain valid.
function Assert-FlitzZipNativeRegistrationCondition {
    param([string]$Text, [string]$Source)
    $conditions = [Collections.Generic.List[bool]]::new()
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -match '^\s*#\s*(if|ifdef|ifndef)\b(?<condition>.*)$') {
            $directive = $Matches[1]
            $condition = $Matches['condition'].Trim()
            $known = ($directive -eq 'ifdef' -and $condition -eq '_WIN32') -or
                ($directive -eq 'if' -and $condition -match '^defined\s*\(\s*_WIN32\s*\)$')
            $conditions.Add(-not $known)
        } elseif ($line -match '^\s*#\s*(else|elif)\b') {
            if (-not $conditions.Count) { throw "Unbalanced native test condition: $Source" }
            $conditions[$conditions.Count - 1] = $true
        } elseif ($line -match '^\s*#\s*endif\b') {
            if (-not $conditions.Count) { throw "Unbalanced native test condition: $Source" }
            $conditions.RemoveAt($conditions.Count - 1)
        } elseif ($line -match '^\s*TEST_CASE\s*\(' -and $conditions.Contains($true)) {
            throw "Native test registration has an unsupported build condition: $Source"
        }
    }
    if ($conditions.Count) { throw "Unbalanced native test condition: $Source" }
}

# Purpose: Read the Windows release target's initial and conditional native test sources.
# Inputs: Root is a checkout containing the CMake object target and TEST_CASE registrations.
# Outputs: Returns unique source/name records; rejects unsupported target statements and duplicate registrations.
function Get-FlitzZipNativeTestInventory {
    param([string]$Root = (Split-Path -Parent $PSScriptRoot))
    $cmake = Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw
    $target = [regex]::Match($cmake, '(?s)add_library\(\s*flitzzip_test_objects\s+OBJECT(?<sources>.*?)\)')
    if (-not $target.Success) { throw 'Cannot resolve the native test object target; review its registration.' }
    $extensions = @([regex]::Matches($cmake, '(?ms)^\s*target_sources\(\s*flitzzip_test_objects\s+PRIVATE(?<sources>.*?)\)'))
    $targetStatements = @([regex]::Matches($cmake, '(?m)^\s*target_sources\(\s*flitzzip_test_objects\b'))
    if ($extensions.Count -ne $targetStatements.Count) {
        throw 'Unsupported native test object target_sources statement; review its registration.'
    }
    $sourceTexts = @($target.Groups['sources'].Value) + @($extensions | ForEach-Object { $_.Groups['sources'].Value })
    $sources = @($sourceTexts | ForEach-Object { [regex]::Matches($_, 'tests/cpp/[\w/-]+\.cpp') |
            ForEach-Object { $_.Value } } | Sort-Object -Unique)
    if (-not $sources.Count) { throw 'Native test object target contains no registered test sources.' }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($source in $sources) {
        $text = Get-Content -LiteralPath (Join-Path $Root $source) -Raw
        Assert-FlitzZipNativeRegistrationCondition -Text $text -Source $source
        $cases = [regex]::Matches($text, '(?m)^TEST_CASE\((?<name>\w+)\)\s*\{')
        if ($cases.Count -ne [regex]::Matches($text, '(?m)^\s*TEST_CASE\s*\(').Count) {
            throw "Native test source contains unsupported registration syntax: $source"
        }
        if (-not $cases.Count -and $source -ne 'tests/cpp/test_main.cpp') {
            throw "Registered test source has no recognized cases: $source"
        }
        foreach ($case in $cases) {
            $name = $case.Groups['name'].Value
            if (-not $names.Add($name)) { throw "Duplicate native test registration: $name" }
            [pscustomobject]@{ source = $source; name = $name }
        }
    }
}

# Purpose: Select reviewed native mechanisms and changed test sources without guessing dependency coverage.
# Inputs: Paths are normalized checkout-relative changes; Full explicitly requests broad qualification.
# Outputs: Returns a deduplicated component cohort or the unchanged full driver for unmapped native inputs.
function Get-FlitzZipNativeTestSelection {
    param([string[]]$Paths, [switch]$Full, [string]$Root = (Split-Path -Parent $PSScriptRoot))
    $broad = [pscustomobject]@{ mode = 'full'; components = @(); tests = @(); requireHip = $false }
    if ($Full.IsPresent) { return $broad }
    $definitions = @(
        @{ id = 'folder-size-cache'; requireHip = $false; paths = @('src/app/folder_size_cache.hpp')
           rules = @(@{ source = 'tests/cpp/test_folder_size_cache.cpp'; pattern = '^' }) },
        @{ id = 'gui-monitor'; requireHip = $false; paths = @(
               'src/app/cpu_sampling.hpp', 'src/app/pdh_percentage_sampler.hpp',
               'src/app/performance_counters.hpp', 'src/app/graph_series.hpp', 'src/app/main_window_monitor.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_system_layout.cpp'; pattern = '^' },
               @{ source = 'tests/cpp/test_resource_usage.cpp'; pattern = '^' }) },
        @{ id = 'sevenzip-parser'; requireHip = $false; paths = @('third_party/lzma_sdk/C/7zArcIn.c')
           rules = @(
               @{ source = 'tests/cpp/test_sevenzip_compat.cpp'; pattern = '^' },
               @{ source = 'tests/cpp/test_sevenzip_ppmd.cpp'; pattern = '^' }) },
        @{ id = 'hip-entropy-encode'; requireHip = $true; paths = @('src/gpu/hip_codec_static_prefix.hip.cpp', 'src/gpu/hip_codec_adaptive_prefix.hip.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_flzip_gpu_prefix.cpp'; pattern = '^flzip_(prefix_reference|gpu_(prefix_|huffman_|entropy_efforts_|efforts_preserve_dictionary_)|required_gpu_(prefix_|huffman_))' },
               @{ source = 'tests/cpp/test_gpu_block_batch.cpp'; pattern = '^gpu_block_batch_hip_identity$' }) },
        @{ id = 'hip-sparse-candidate'; requireHip = $true; paths = @('src/gpu/sparse_pattern_candidate.cpp', 'src/gpu/hip_sparse_pattern.hip.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_sparse_pattern_block.cpp'; pattern = '^(sparse_pattern_|long_sparse_pattern_)' },
               @{ source = 'tests/cpp/test_archive_roundtrip.cpp'; pattern = '^flzip_(sparse_pattern_cpu_reader_roundtrip|long_sparse_pattern_v7_reader_roundtrip|long_sparse_pattern_writer_selects_v7)$' },
               @{ source = 'tests/cpp/test_flzip_gpu_prefix.cpp'; pattern = '^flzip_gpu_(entropy_efforts_preserve_per_block_winners|efforts_preserve_dictionary_candidates_after_entropy_gain)$' }) },
        @{ id = 'hip-dictionary-codec'; requireHip = $true; paths = @('src/gpu/hip_dictionary_matcher.hip.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_dictionary_matcher.cpp'; pattern = '^dictionary_(?!.*_(benchmark|reference)_opt_in$|interop_export_)' },
               @{ source = 'tests/cpp/test_archive_roundtrip.cpp'; pattern = '^flzip_(gpu_dictionary_(writer_levels_and_roundtrip|mixed_and_archive_roundtrip)|required_gpu_encoder_emits_no_cpu_deflate_blocks)$' },
               @{ source = 'tests/cpp/test_flzip_gpu_prefix.cpp'; pattern = '^flzip_gpu_(entropy_efforts_preserve_per_block_winners|efforts_preserve_dictionary_candidates_after_entropy_gain|prefix_candidate_selection_is_per_block|prefix_levels_compact_shifted_alphabet)$' }) },
        @{ id = 'hip-dictionary-candidate'; requireHip = $true; paths = @('src/gpu/dictionary_candidate.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_dictionary_matcher.cpp'; pattern = '^dictionary_(segmented_records_large_block_roundtrip|off_grid_segmented_records_roundtrip|adjacent_periodic_blocks_preserve_encoded_bytes|periodic_group_probe_falls_back_without_byte_changes|periodic_efforts_preserve_size_order|periodic_candidate_independent_block_decode|mixed_periodic_distances_roundtrip|production_chunk_crc_and_decode)$' },
               @{ source = 'tests/cpp/test_archive_roundtrip.cpp'; pattern = '^flzip_(gpu_dictionary_(writer_levels_and_roundtrip|mixed_and_archive_roundtrip)|required_gpu_encoder_emits_no_cpu_deflate_blocks)$' },
               @{ source = 'tests/cpp/test_flzip_gpu_prefix.cpp'; pattern = '^flzip_gpu_(entropy_efforts_preserve_per_block_winners|efforts_preserve_dictionary_candidates_after_entropy_gain|prefix_candidate_selection_is_per_block|prefix_levels_compact_shifted_alphabet)$' }) }
    )
    $native = @($Paths | Where-Object { $_ -match '^(src|tests|fuzz)/|^CMakeLists\.txt$|^cmake/|^third_party/' })
    if (-not $native.Count) { return $broad }
    if ($native.Count -eq 1 -and $native[0] -eq 'tests/cpp/test_main.cpp') {
        return [pscustomobject]@{ mode = 'runner'; components = @('native-test-runner'); tests = @(); requireHip = $false }
    }
    $components = @($definitions | Where-Object { @($_.paths | Where-Object { $_ -in $native }).Count -gt 0 })
    $mapped = @($components | ForEach-Object { $_.paths })
    $testSources = @($native | Where-Object { $_ -match '^tests/cpp/test_\w+\.cpp$' -and $_ -ne 'tests/cpp/test_main.cpp' })
    if (@($native | Where-Object { $_ -notin $mapped -and $_ -notin $testSources }).Count) { return $broad }
    $inventory = @(Get-FlitzZipNativeTestInventory -Root $Root)
    foreach ($source in $testSources) {
        if ($source -notin $inventory.source) { return $broad }
    }
    $selected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($component in $components) {
        foreach ($rule in $component.rules) {
            $matchedCases = @($inventory | Where-Object { $_.source -eq $rule.source -and $_.name -match $rule.pattern })
            if (-not $matchedCases.Count) { throw "Native component has no matching tests: $($component.id) / $($rule.source)" }
            foreach ($case in $matchedCases) { [void]$selected.Add($case.name) }
        }
    }
    foreach ($test in @($inventory | Where-Object { $_.source -in $testSources })) { [void]$selected.Add($test.name) }
    if (-not $selected.Count) { throw 'Native component selection is empty; review changed mechanisms.' }
    # Classify actual changed sources conservatively, including newly registered device suites.
    # A build guard or capability probe can skip assertions; admission must surround their execution.
    $hipSources = @($testSources | Where-Object {
        $text = Get-Content -LiteralPath (Join-Path $Root $_) -Raw
        $text -match '(?m)^\s*#\s*(?:if|ifdef|ifndef|elif)\b[^\r\n]*\bFLITZZIP_ENABLE_HIP\b|\b(?:query_gpu_info|hip_kernel_api|hip_device_available|require_hip_device_ready)\s*\('
    })
    return [pscustomobject]@{ mode = 'component'; components = @($components | ForEach-Object { $_.id }) + $testSources
        tests = @($selected | Sort-Object); requireHip = (@($components | Where-Object { $_.requireHip }).Count -gt 0 -or @($testSources | Where-Object { $_ -in $hipSources }).Count -gt 0) }
}

# Purpose: Require a compiled, available HIP backend before and after GPU-dependent component cases.
# Inputs: Cli is receipt-validated; CpuOnlyValidation explicitly requires a backend compiled without HIP.
# Outputs: Requires actual HIP readiness normally, or rejects any HIP backend in declared CPU-only validation.
function Assert-FlitzZipNativeSelectionHip {
    param([Parameter(Mandatory = $true)][string]$Cli, [switch]$CpuOnlyValidation)
    $info = @(& $Cli gpu-info)
    if ($CpuOnlyValidation.IsPresent) {
        if ($LASTEXITCODE -ne 1 -or $info -notcontains 'hip_compiled=false' -or $info -notcontains 'available=false') {
            throw 'CPU-only native validation requires a CPU-only binary; GPU qualification cannot be bypassed.'
        }
        return
    }
    if ($LASTEXITCODE -ne 0 -or $info -notcontains 'hip_compiled=true' -or $info -notcontains 'available=true') {
        throw 'Native HIP component selection requires a compiled HIP backend and an available device.'
    }
}

# Purpose: Execute exact cases once, optionally retaining ordinary assertion failures for a development batch.
# Inputs: Validated selection/binaries; CpuOnlyValidation requires HIP OFF. The caller owns process-tree deadlines.
# Outputs: Throws for any failure; batch mode continues only after a complete one-case assertion result and retained
# backend readiness. Registry disagreement, process failure and unavailable hardware always stop execution.
function Invoke-FlitzZipNativeTestSelection {
    param([Parameter(Mandatory = $true)]$Selection,
          [Parameter(Mandatory = $true)][string]$TestRunner, [string]$Cli, [switch]$CpuOnlyValidation,
          [switch]$ContinueAfterAssertionFailure)
    if ($Selection.mode -ne 'component' -or -not $Selection.tests.Count -or
        @($Selection.tests | Sort-Object -Unique).Count -ne $Selection.tests.Count) {
        throw 'Native component execution requires a nonempty unique selection.'
    }
    $failures = [Collections.Generic.List[string]]::new()
    $requiresBackendCheck = $Selection.requireHip -or $CpuOnlyValidation.IsPresent
    $backendCheckCurrent = $false
    if ($Selection.requireHip -or $CpuOnlyValidation.IsPresent) { Assert-FlitzZipNativeSelectionHip -Cli $Cli -CpuOnlyValidation:$CpuOnlyValidation }
    foreach ($name in $Selection.tests) {
        $output = @(& $TestRunner "=$name")
        $exitCode = $LASTEXITCODE
        $output
        $backendCheckCurrent = $false
        $ran = @($output | Where-Object { $_ -match '^\[RUN \] ' })
        $summaries = @($output | Where-Object { $_ -match '^\d+ tests, \d+ failed$' })
        $complete = $ran.Count -eq 1 -and $ran[0] -ceq "[RUN ] $name" -and $summaries.Count -eq 1
        if ($complete -and $exitCode -eq 0 -and $summaries[0] -ceq '1 tests, 0 failed') { continue }
        if ($ContinueAfterAssertionFailure.IsPresent -and $complete -and $exitCode -eq 1 -and
            $summaries[0] -ceq '1 tests, 1 failed') {
            $failures.Add($name)
            if ($requiresBackendCheck) {
                Assert-FlitzZipNativeSelectionHip -Cli $Cli -CpuOnlyValidation:$CpuOnlyValidation
                $backendCheckCurrent = $true
            }
            continue
        }
        throw "Selected native case did not run exactly once and pass: $name (exit $exitCode)."
    }
    if ($requiresBackendCheck -and -not $backendCheckCurrent) {
        Assert-FlitzZipNativeSelectionHip -Cli $Cli -CpuOnlyValidation:$CpuOnlyValidation
    }
    if ($failures.Count) { throw "Native batch assertion failures: $($failures -join ', ')." }
    if ($CpuOnlyValidation.IsPresent) {
        Write-Output "CPU-only component validation completed: $($Selection.tests.Count) invocations; GPU assertions are not qualified."
    } else {
        Write-Output "Native component tests passed: $($Selection.tests.Count) cases; components=$($Selection.components -join ',')."
    }
}

# Purpose: Prevent hosted CPU validation from weakening a required-HIP product invocation.
# Inputs: Receipt is already validated against current files and output bytes.
# Outputs: Rejects CPU-only mode unless the successful build explicitly configured HIP OFF.
function Assert-FlitzZipCpuValidationReceipt {
    param([Parameter(Mandatory = $true)]$Receipt)
    if ($Receipt.recipe.FLITZZIP_ENABLE_HIP -ne 'OFF') {
        throw 'CPU-only native validation requires a receipt configured with HIP OFF.'
    }
}
