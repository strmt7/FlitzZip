$ErrorActionPreference = 'Stop'
$script:negativeControlCount = 0
. (Join-Path $PSScriptRoot 'zstd_rewrite_policy.ps1')
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $fixtureParent ('superzip-zstd-policy-' + [guid]::NewGuid().ToString('N'))

# Purpose: Require a safety guard to reject one recurrence in the real owned source.
# Inputs: Path and exact replacement form one fixture mutation; Cause binds the expected rejection.
# Outputs: Throws for unexpected acceptance/rejection and restores the fixture on every exit.
function Assert-ZstdPolicyMutation {
    param([string]$Path, [string]$Original, [string]$Replacement, [string]$Cause)

    $file = Join-Path $fixtureRoot $Path
    $text = Get-Content -LiteralPath $file -Raw
    if (-not $text.Contains($Original)) { throw "Mutation fixture drift: $Path/$Original" }
    try {
        [IO.File]::WriteAllText($file, $text.Replace($Original, $Replacement))
        $rejected = $false
        try { Assert-ZstdRewritePolicy -RepoRoot $fixtureRoot }
        catch {
            if ($_.Exception.Message -notlike "*$Cause*") { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "Zstandard policy accepted recurrence: $Cause" }
        $script:negativeControlCount += 1
    } finally {
        [IO.File]::WriteAllText($file, $text)
    }
}

try {
    foreach ($relative in @('cmake/ZstdRawBlockWriter.c', 'cmake/ZstdCoverSelection.cpp', 'cmake/ZstdDictionaryBounds.cmake', 'cmake/PatchZstdLegacy.cmake',
            'cmake/ZstdLegacyPublicStream.c', 'tests/cpp/test_zstd_bounds.cpp',
            'tests/cpp/test_zstd_cover_selection.cpp', 'tests/cpp/test_zstd_legacy_failures.cpp',
            'cmake/ZstdLegacyStreamV05.c', 'cmake/ZstdLegacyStreamV06.c', 'cmake/ZstdLegacyStreamV07.c',
            'cmake/ZstdLegacyHistoryV05.c', 'cmake/ZstdLegacyHistoryV06.c', 'cmake/ZstdLegacyHistoryV07.c',
            'cmake/ZstdLegacyBlockV07.c', 'cmake/ZstdLegacyDictionary.cmake', 'tests/cpp/test_zstd_legacy_history.cpp')) {
        $destination = Join-Path $fixtureRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repoRoot $relative) -Destination $destination
    }
    Assert-ZstdRewritePolicy -RepoRoot $fixtureRoot
    foreach ($mutation in @(
            @('cmake/ZstdRawBlockWriter.c', 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize', 'srcSize + ZSTD_blockHeaderSize > dstCapacity', 'raw-block extent'),
            @('cmake/ZstdCoverSelection.cpp', 'candidateContentSize <= initialized.size()', 'candidateContentSize <= largestDictSize', 'initialized dictionary extent'),
            @('cmake/ZstdCoverSelection.cpp', 'candidateDictSize > initialized.size() / 2', 'candidateDictSize * 2 > initialized.size()', 'dictionary growth overflow'),
            @('cmake/ZstdCoverSelection.cpp', 'content.initializedOffset > content.capacity', 'content.initializedOffset > 0', 'dictionary allocation geometry'),
            @('cmake/ZstdCoverSelection.cpp', 'content.capacity > static_cast<std::size_t>(PTRDIFF_MAX)', 'content.capacity > 0', 'dictionary pointer extent'),
            @('cmake/ZstdCoverSelection.cpp', 'using DictionaryOwner = std::unique_ptr<BYTE[], DictionaryDelete>;', 'using DictionaryOwner = BYTE*;', 'dictionary scoped ownership'),
            @('cmake/ZstdCoverSelection.cpp', 'const auto suffix = initialized.last(candidateContentSize);', 'const auto suffix = allocation.last(candidateContentSize);', 'dictionary bounded suffix'),
            @('cmake/ZstdDictionaryBounds.cmake', 'COVER_freeSelectedDictionary(selection.dictContent);', 'free(selection.dictContent);', 'dictionary matching release'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_raw_block_writer("${source_dir}")', '# omitted raw-block patch', 'raw-block patch dispatch'),
            @('cmake/ZstdLegacyPublicStream.c', '    switch(version)', "    output->dst = legacyContext;`n    switch(version)", 'caller-owned public buffer'),
            @('tests/cpp/test_zstd_bounds.cpp', 'TEST_CASE(zstd_raw_block_writer_overflow_rejection)', 'TEST_CASE(removed_overflow)', 'raw-block overflow regression'))) {
        Assert-ZstdPolicyMutation -Path $mutation[0] -Original $mutation[1] -Replacement $mutation[2] -Cause $mutation[3]
    }
    foreach ($version in @('05', '06', '07')) {
        $history = "cmake/ZstdLegacyHistoryV$version.c"
        Assert-ZstdPolicyMutation -Path $history -Original 'sequence.matchLength > available - sequence.litLength' `
            -Replacement 'sequence.litLength + sequence.matchLength > available' -Cause "v$version sequence extent"
        Assert-ZstdPolicyMutation -Path $history -Original 'dictionaryOffset > dictSize' `
            -Replacement 'dictionaryOffset > 0' -Cause "v$version history extent"
        $fragment = "cmake/ZstdLegacyStreamV$version.c"
        Assert-ZstdPolicyMutation -Path $fragment -Original '*srcSizePtr != 0 ? istart + *srcSizePtr : istart' `
            -Replacement 'istart + *srcSizePtr' -Cause "v$version nullable input extent"
        $cleanup = if ($version -eq '07') { 'zbd->customMem.customFree(zbd->customMem.opaque, replacementIn);' } else { 'free(replacementIn);' }
        Assert-ZstdPolicyMutation -Path $fragment -Original $cleanup `
            -Replacement '/* omitted cleanup */' -Cause "v$version partial allocation cleanup"
    }
    # A comment containing the removed check cannot satisfy the executable-source guard.
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdRawBlockWriter.c' `
        -Original 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize' `
        -Replacement '/* dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize */ 0' `
        -Cause 'raw-block extent'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyBlockV07.c' -Original 'if (ZSTDv07_isError(result))' `
        -Replacement 'if (0)' -Cause 'legacy error history boundary'
    Assert-ZstdPolicyMutation -Path 'cmake/PatchZstdLegacy.cmake' `
        -Original 'superzip_patch_zstd_legacy_history("${source_dir}")' `
        -Replacement '# omitted legacy history patch' -Cause 'legacy history patch dispatch'
    Assert-ZstdPolicyMutation -Path 'cmake/PatchZstdLegacy.cmake' `
        -Original 'superzip_patch_zstd_legacy_dictionary("${source_dir}")' `
        -Replacement '# omitted legacy dictionary patch' -Cause 'legacy dictionary patch dispatch'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyDictionary.cmake' `
        -Original 'if (ZSTD@version@_isError(initialized)) return initialized;' `
        -Replacement '(void)initialized;' -Cause 'legacy dictionary error propagation'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyDictionary.cmake' `
        -Original 'if (ZSTD@version@_isError(initialized)) return initialized;' `
        -Replacement '/* if (ZSTD@version@_isError(initialized)) return initialized; */' -Cause 'legacy dictionary error propagation'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyDictionary.cmake' `
        -Original 'dictSize < sizeof(U32) ||' `
        -Replacement '' -Cause 'legacy dictionary marker extent'
    Assert-ZstdPolicyMutation -Path 'tests/cpp/test_zstd_bounds.cpp' `
        -Original 'TEST_CASE(zstd_legacy_v05_dictionary_error_propagation)' `
        -Replacement 'TEST_CASE(removed_dictionary_error)' -Cause 'v05 dictionary error regression'
    foreach ($entry in @(
            @('superzip_patch_zstd_raw_block_writer', 'raw-block patch dispatch'),
            @('superzip_patch_zstd_legacy_public_stream', 'public stream patch dispatch'),
            @('superzip_patch_zstd_legacy_history', 'legacy history patch dispatch'),
            @('superzip_patch_zstd_dictionary_bounds', 'dictionary bounds patch dispatch'),
            @('superzip_patch_zstd_legacy_dictionary', 'legacy dictionary patch dispatch'))) {
        $dispatch = $entry[0] + '("${source_dir}")'
        Assert-ZstdPolicyMutation -Path 'cmake/PatchZstdLegacy.cmake' -Original $dispatch `
            -Replacement ('# ' + $dispatch) -Cause $entry[1]
    }
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'cmake/ZstdRawBlockWriter.c')
    $missingRejected = $false
    try { Assert-ZstdRewritePolicy -RepoRoot $fixtureRoot }
    catch { if ($_.Exception.Message -notlike '*ZstdRawBlockWriter.c*') { throw }; $missingRejected = $true }
    if (-not $missingRejected) { throw 'Missing source guard was accepted.' }
    Write-Output "Zstandard rewrite policy passed positive source checks and $($script:negativeControlCount) mutation controls, plus missing-source rejection."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $prefix = $fixtureParent.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^superzip-zstd-policy-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the owned Zstandard policy fixture.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
