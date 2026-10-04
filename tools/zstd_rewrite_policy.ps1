# Purpose: Reject recurrence of the concrete Zstandard source defects repaired here.
# Inputs: RepoRoot owns the patch fragments and their production-facing regressions.
# Outputs: Throws with the missing safety boundary; this static guard does not replace native tests or SAST.
function Assert-ZstdRewritePolicy {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $requirements = @(
        @('cmake/ZstdRawBlockWriter.c', 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize', 'raw-block extent'),
        @('cmake/ZstdCoverSelection.cpp', 'candidateContentSize <= initialized.size()', 'initialized dictionary extent'),
        @('cmake/ZstdCoverSelection.cpp', 'candidateDictSize > initialized.size() / 2', 'dictionary growth overflow'),
        @('cmake/ZstdCoverSelection.cpp', 'content.initializedOffset > content.capacity', 'dictionary allocation geometry'),
        @('cmake/ZstdCoverSelection.cpp', 'content.capacity > static_cast<std::size_t>(PTRDIFF_MAX)', 'dictionary pointer extent'),
        @('cmake/ZstdCoverSelection.cpp', 'using DictionaryOwner = std::unique_ptr<BYTE[], DictionaryDelete>;', 'dictionary scoped ownership'),
        @('cmake/ZstdCoverSelection.cpp', 'const auto suffix = initialized.last(candidateContentSize);', 'dictionary bounded suffix'),
        @('cmake/ZstdDictionaryBounds.cmake', 'COVER_freeSelectedDictionary(selection.dictContent);', 'dictionary matching release'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_dictionary_bounds("${source_dir}")', 'dictionary bounds patch dispatch'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_raw_block_writer("${source_dir}")', 'raw-block patch dispatch'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_legacy_public_stream("${source_dir}")', 'public stream patch dispatch'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_legacy_history("${source_dir}")', 'legacy history patch dispatch'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_legacy_dictionary("${source_dir}")', 'legacy dictionary patch dispatch'),
        @('cmake/ZstdLegacyDictionary.cmake', 'if (ZSTD@version@_isError(initialized)) return initialized;', 'legacy dictionary error propagation'),
        @('cmake/ZstdLegacyDictionary.cmake', 'dictSize < sizeof(U32) ||', 'legacy dictionary marker extent'),
        @('cmake/ZstdLegacyBlockV07.c', 'if (ZSTDv07_isError(result))', 'legacy error history boundary'),
        @('tests/cpp/test_zstd_legacy_history.cpp', 'TEST_CASE(zstd_legacy_sequence_rejects_malformed_extents)', 'legacy sequence extent regression'),
        @('cmake/PatchZstdLegacy.cmake', 'Zstandard patch source identity mismatch', 'patch input identity'),
        @('cmake/PatchZstdLegacy.cmake', 'Zstandard patch output identity mismatch', 'patch output identity'),
        @('tests/cpp/test_zstd_bounds.cpp', 'TEST_CASE(zstd_raw_block_writer_overflow_rejection)', 'raw-block overflow regression'),
        @('tests/cpp/test_zstd_cover_selection.cpp', 'TEST_CASE(zstd_cover_shrinking_preserves_initialized_extent)', 'dictionary input-span regression'),
        @('tests/cpp/test_zstd_cover_selection.cpp', 'TEST_CASE(zstd_cover_selection_failure_ownership)', 'dictionary allocation regression'),
        @('tests/cpp/test_zstd_cover_selection.cpp', 'TEST_CASE(zstd_cover_selection_rejects_invalid_allocation_geometry)', 'dictionary geometry regression'),
        @('tests/cpp/test_zstd_legacy_failures.cpp', 'TEST_CASE(zstd_legacy_stream_empty_buffers_preserve_progress)', 'empty-buffer progress regression')
    )
    foreach ($version in @('05', '06', '07')) {
        $history = "cmake/ZstdLegacyHistoryV$version.c"
        $requirements += ,@($history, 'sequence.matchLength > available - sequence.litLength', "v$version sequence extent")
        $requirements += ,@($history, 'dictionaryOffset > dictSize', "v$version history extent")
        $fragment = "cmake/ZstdLegacyStreamV$version.c"
        $requirements += ,@($fragment, 'if (begin == NULL)', "v$version empty cursor")
        $requirements += ,@($fragment, '*srcSizePtr != 0 ? istart + *srcSizePtr : istart', "v$version nullable input extent")
        $outputSize = if ($version -eq '05') { 'maxDstSizePtr' } else { 'dstCapacityPtr' }
        $requirements += ,@($fragment, "*$outputSize != 0 ? ostart + *$outputSize : ostart", "v$version nullable output extent")
        $cleanup = if ($version -eq '07') { 'zbd->customMem.customFree(zbd->customMem.opaque, replacementIn);' } else { 'free(replacementIn);' }
        $requirements += ,@($fragment, $cleanup, "v$version partial allocation cleanup")
        $requirements += ,@('tests/cpp/test_zstd_bounds.cpp', "TEST_CASE(zstd_legacy_v${version}_public_output_backpressure)", "v$version public pointer regression")
        $requirements += ,@('tests/cpp/test_zstd_bounds.cpp', "TEST_CASE(zstd_legacy_v${version}_dictionary_error_propagation)", "v$version dictionary error regression")
        $requirements += ,@('tests/cpp/test_zstd_bounds.cpp', "TEST_CASE(zstd_legacy_v${version}_short_raw_dictionary)", "v$version short dictionary regression")
        $requirements += ,@('tests/cpp/test_zstd_legacy_failures.cpp', "TEST_CASE(zstd_legacy_v${version}_stream_output_allocation_failure)", "v$version allocation failure regression")
        $requirements += ,@('tests/cpp/test_zstd_legacy_failures.cpp', "TEST_CASE(zstd_legacy_v${version}_stream_growth_preserves_buffers)", "v$version replacement ownership regression")
    }
    foreach ($requirement in $requirements) {
        $path = Join-Path $RepoRoot $requirement[0]
        $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
        # Safety boundaries must exist in executable source, not only in explanatory comments.
        $code = [regex]::Replace($text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
        $present = $code.Contains($requirement[1])
        if ($requirement[0] -eq 'cmake/PatchZstdLegacy.cmake' -and $requirement[2] -like '*dispatch') {
            # A comment retaining the call text cannot stand in for an executable dispatch line.
            $call = '(?m)^\s*' + [regex]::Escape($requirement[1]) + '\s*$'
            $present = [regex]::IsMatch($code, $call)
        }
        if (-not $present) {
            throw "Zstandard rewrite policy missing $($requirement[2]): $($requirement[0])"
        }
    }
    $public = Get-Content -LiteralPath (Join-Path $RepoRoot 'cmake/ZstdLegacyPublicStream.c') -Raw
    $shipped = [regex]::Replace($public, '(?ms)^#if \(ZSTD_LEGACY_SUPPORT <= 4\).*?^#endif\s*', '')
    $shipped = [regex]::Replace($shipped, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
    if ($shipped -match '(input->src|output->dst)\s*=(?!=)') {
        throw 'Zstandard rewrite policy rejects replacement of caller-owned public buffer pointers.'
    }
}
