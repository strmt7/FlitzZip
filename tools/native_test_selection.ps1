# Purpose: Decode canonical changed-path metadata consistently across Windows PowerShell and PowerShell 7.
# Inputs: Base64 contains a UTF-8 JSON array of normalized checkout-relative path strings.
# Outputs: Returns path strings; rejects malformed encoding, non-array JSON and unsafe or empty paths.
function ConvertFrom-SuperZipNativeSelectionPath {
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

# Purpose: Read the authoritative sources registered in the main native test target.
# Inputs: Root is a checkout containing the existing CMake object target and TEST_CASE registrations.
# Outputs: Returns unique source/name records; rejects missing registrations and duplicate names.
function Get-SuperZipNativeTestInventory {
    param([string]$Root = (Split-Path -Parent $PSScriptRoot))
    $cmake = Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw
    $target = [regex]::Match($cmake, '(?s)add_library\(\s*superzip_test_objects\s+OBJECT(?<sources>.*?)\)')
    if (-not $target.Success) { throw 'Cannot resolve the native test object target; review its registration.' }
    $sources = @([regex]::Matches($target.Groups['sources'].Value, 'tests/cpp/[\w/-]+\.cpp') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    if (-not $sources.Count) { throw 'Native test object target contains no registered test sources.' }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($source in $sources) {
        $text = Get-Content -LiteralPath (Join-Path $Root $source) -Raw
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
function Get-SuperZipNativeTestSelection {
    param([string[]]$Paths, [switch]$Full, [string]$Root = (Split-Path -Parent $PSScriptRoot))
    $broad = [pscustomobject]@{ mode = 'full'; components = @(); tests = @(); requireHip = $false }
    if ($Full.IsPresent) { return $broad }
    $definitions = @(
        @{ id = 'hip-entropy-encode'; paths = @('src/gpu/hip_codec_static_prefix.hip.cpp', 'src/gpu/hip_codec_adaptive_prefix.hip.cpp')
           rules = @(
               @{ source = 'tests/cpp/test_suzip_gpu_prefix.cpp'; pattern = '^suzip_(prefix_reference|gpu_(prefix_|huffman_|entropy_efforts_|efforts_preserve_dictionary_)|required_gpu_(prefix_|huffman_))' },
               @{ source = 'tests/cpp/test_gpu_block_batch.cpp'; pattern = '^gpu_block_batch_hip_identity$' }) }
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
    $inventory = @(Get-SuperZipNativeTestInventory -Root $Root)
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
    $hipSources = @('tests/cpp/test_suzip_gpu_prefix.cpp', 'tests/cpp/test_gpu_block_batch.cpp',
        'tests/cpp/test_dictionary_matcher.cpp', 'tests/cpp/test_sparse_pattern_block.cpp')
    return [pscustomobject]@{ mode = 'component'; components = @($components | ForEach-Object { $_.id }) + $testSources
        tests = @($selected | Sort-Object); requireHip = ($components.Count -gt 0 -or @($testSources | Where-Object { $_ -in $hipSources }).Count -gt 0) }
}

# Purpose: Require a compiled, available HIP backend before and after GPU-dependent component cases.
# Inputs: Cli is receipt-validated; CpuOnlyValidation explicitly requires a backend compiled without HIP.
# Outputs: Requires actual HIP readiness normally, or rejects any HIP backend in declared CPU-only validation.
function Assert-SuperZipNativeSelectionHip {
    param([Parameter(Mandatory = $true)][string]$Cli, [switch]$CpuOnlyValidation)
    $info = @(& $Cli gpu-info)
    if ($CpuOnlyValidation.IsPresent) {
        if ($LASTEXITCODE -ne 0 -or $info -notcontains 'hip_compiled=false' -or $info -notcontains 'available=false') {
            throw 'CPU-only native validation requires a CPU-only binary; GPU qualification cannot be bypassed.'
        }
        return
    }
    if ($LASTEXITCODE -ne 0 -or $info -notcontains 'hip_compiled=true' -or $info -notcontains 'available=true') {
        throw 'Native HIP component selection requires a compiled HIP backend and an available device.'
    }
}

# Purpose: Execute each selected native case once and stop immediately on failure or registry disagreement.
# Inputs: Selection and binaries are validated; CpuOnlyValidation requires an explicit HIP-OFF build receipt.
# Outputs: Confirms exact cases and backend state; CPU-only completion never qualifies GPU assertions.
function Invoke-SuperZipNativeTestSelection {
    param([Parameter(Mandatory = $true)]$Selection,
          [Parameter(Mandatory = $true)][string]$TestRunner, [string]$Cli, [switch]$CpuOnlyValidation)
    if ($Selection.mode -ne 'component' -or -not $Selection.tests.Count -or
        @($Selection.tests | Sort-Object -Unique).Count -ne $Selection.tests.Count) {
        throw 'Native component execution requires a nonempty unique selection.'
    }
    if ($Selection.requireHip -or $CpuOnlyValidation.IsPresent) { Assert-SuperZipNativeSelectionHip -Cli $Cli -CpuOnlyValidation:$CpuOnlyValidation }
    foreach ($name in $Selection.tests) {
        $output = @(& $TestRunner "=$name")
        $exitCode = $LASTEXITCODE
        $output
        $ran = @($output | Where-Object { $_ -match '^\[RUN \] ' })
        if ($exitCode -ne 0 -or $output -notcontains '1 tests, 0 failed' -or
            $ran.Count -ne 1 -or $ran[0] -ne "[RUN ] $name") {
            throw "Selected native case did not run exactly once and pass: $name (exit $exitCode)."
        }
    }
    if ($Selection.requireHip -or $CpuOnlyValidation.IsPresent) { Assert-SuperZipNativeSelectionHip -Cli $Cli -CpuOnlyValidation:$CpuOnlyValidation }
    if ($CpuOnlyValidation.IsPresent) {
        Write-Output "CPU-only component validation completed: $($Selection.tests.Count) invocations; GPU assertions are not qualified."
    } else {
        Write-Output "Native component tests passed: $($Selection.tests.Count) cases; components=$($Selection.components -join ',')."
    }
}

# Purpose: Prevent hosted CPU validation from weakening a required-HIP product invocation.
# Inputs: Receipt is already validated against current files and output bytes.
# Outputs: Rejects CPU-only mode unless the successful build explicitly configured HIP OFF.
function Assert-SuperZipCpuValidationReceipt {
    param([Parameter(Mandatory = $true)]$Receipt)
    if ($Receipt.recipe.SUPERZIP_ENABLE_HIP -ne 'OFF') {
        throw 'CPU-only native validation requires a receipt configured with HIP OFF.'
    }
}
