# Purpose: Validate executable safety requirements against their actual sources.
# Inputs: RepoRoot and path/token/diagnostic triples identify required boundaries.
# Outputs: Rejects missing source or tokens; comments cannot satisfy executable dispatch.
function Assert-ZstdSourceRequirement {
    param([string]$RepoRoot, [object[]]$Requirements)

    foreach ($requirement in $Requirements) {
        $path = Join-Path $RepoRoot $requirement[0]
        $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
        $code = [regex]::Replace($text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
        $present = $code.Contains($requirement[1])
        if ($requirement[0] -eq 'cmake/PatchZstdLegacy.cmake' -and $requirement[2] -like '*dispatch') {
            $call = '(?m)^\s*' + [regex]::Escape($requirement[1]) + '\s*$'
            $present = [regex]::IsMatch($code, $call)
        }
        if (-not $present) {
            throw "Zstandard rewrite policy missing $($requirement[2]): $($requirement[0])"
        }
    }
}

# Purpose: Reject recurrence of the concrete Zstandard source defects repaired here.
# Inputs: RepoRoot owns the patch fragments and their production-facing regressions.
# Outputs: Throws with the missing safety boundary; this static guard does not replace native tests or SAST.
function Assert-ZstdRewritePolicy {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $requirements = @(
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_header_components("${source_dir}")', 'guarded header component dispatch'),
        @('cmake/ZstdHeaderComponents.cmake', 'if(NOT component_hash STREQUAL "${${key}_${part}_hash}")', 'complete header component identity'),
        @('cmake/ZstdHeaderComponents.cmake', 'if(EXISTS "${temporary}" OR EXISTS "${raw}")', 'header interrupted publication rejection'),
        @('tests/zstd/headers/header_contract.c', '#define XXH_INLINE_ALL', 'late hash implementation opt-in'),
        @('cmake/ZstdAlgorithmProgress.cmake', 'int prefixSearchComplete = 0;', 'match budget and termination separation'),
        @('cmake/ZstdAlgorithmProgress.cmake', '&& !prefixSearchComplete)', 'match completed-prefix dictionary gate'),
        @('cmake/ZstdSuffixRanks.c', 'if (suffix < 0 || suffix >= count)', 'suffix rank index extent'),
        @('cmake/ZstdSuffixRanks.c', 'if (remaining == 0)', 'suffix group termination'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_algorithm_progress("${source_dir}")', 'algorithm progress patch dispatch'),
        @('tests/cpp/test_zstd_algorithm_progress.cpp', 'TEST_CASE(zstd_suffix_progress_matches_lexicographic_oracle)', 'suffix independent ordering oracle'),
        @('tests/cpp/test_zstd_algorithm_progress.cpp', 'TEST_CASE(zstd_suffix_progress_rejects_malformed_groups)', 'suffix malformed group regression'),
        @('tests/cpp/test_zstd_match_progress.cpp', 'TEST_CASE(zstd_match_progress_preserves_attached_dictionary_frames)', 'match pre-rewrite frame controls'),
        @('cmake/ZstdCoverWorkGroup.cpp', 'POOL_joinJobs(pool.get());', 'optimizer actual worker completion'),
        @('cmake/ZstdCoverWorkGroup.cmake', '#if PTRDIFF_MAX <= UINT32_MAX', 'sample count architecture range'),
        @('cmake/ZstdCoverWorkGroup.cpp', 'COVER_best_wait(&best);', 'optimizer logical completion'),
        @('cmake/ZstdCoverWorkGroup.cpp', 'if (committed && destroyContext != nullptr)', 'optimizer initialized context cleanup'),
        @('cmake/ZstdCoverWorkGroup.cpp', 'using ContextOwner = std::unique_ptr<std::byte[], ContextDelete>;', 'optimizer exclusive context storage'),
        @('cmake/ZstdCoverWorkGroup.cpp', 'group->best.dictSize > capacity', 'optimizer output extent'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_work_group("${source_dir}")', 'optimizer owner patch dispatch'),
        @('tests/cpp/test_zstd_work_group.cpp', 'TEST_CASE(zstd_work_group_joins_actual_worker_lifetime)', 'optimizer early-completion regression'),
        @('tests/cpp/test_zstd_work_group.cpp', 'TEST_CASE(zstd_work_group_context_transaction_and_failures)', 'optimizer context failure regression'),
        @('cmake/ZstdLegacyBuffers.cpp', 'using BufferOwner = std::unique_ptr<char[], BufferDelete>;', 'legacy exclusive buffer ownership'),
        @('tests/zstd/fault_allocator.cpp', 'std::array<std::unique_ptr<std::byte[]>, 32> allocations;', 'fixture exclusive allocation records'),
        @('cmake/ZstdHuffmanTable.c', 'count > (table.capacity - entries) / length', 'Huffman ranked table capacity'),
        @('cmake/ZstdHuffmanTable.c', 'symbol != symbolCount', 'Huffman complete symbol geometry'),
        @('cmake/ZstdFseTableGeometry.c', 'tableLog > 15 || maxSymbolValue > 255', 'FSE descriptor geometry'),
        @('cmake/ZstdFseTableGeometry.c', 'result.transformOffset = sizeof(FSE_CTableHeader) + result.stateCount * sizeof(U16);', 'FSE region units'),
        @('tests/cpp/test_zstd_fse_table.cpp', 'TEST_CASE(zstd_fse_packed_regions_preserve_layout)', 'FSE independent packed layout oracle'),
        @('tests/cpp/test_zstd_fse_table.cpp', 'TEST_CASE(zstd_fse_packed_regions_reject_invalid_descriptors)', 'FSE malformed descriptor regression'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_table_geometry("${source_dir}")', 'table geometry patch dispatch'),
        @('tests/cpp/test_zstd_huffman_table.cpp', 'TEST_CASE(zstd_huffman_typed_fill_rank_oracle)', 'Huffman independent rank oracle'),
        @('tests/cpp/test_zstd_huffman_table.cpp', 'TEST_CASE(zstd_huffman_typed_fill_rejects_geometry)', 'Huffman malformed table regression'),
        @('cmake/ZstdLegacyBuffers.cpp', 'count > destinationCapacity - destinationOffset || count > sourceCapacity - sourceOffset', 'legacy complete copy geometry'),
        @('cmake/ZstdLegacyBuffers.cpp', 'std::copy_backward(input.begin(), input.end(), output.end());', 'legacy backward overlap transfer'),
        @('cmake/ZstdLegacyBuffers.cpp', 'if (input.data() == output.data())', 'legacy identical-range copy contract'),
        @('cmake/ZstdLegacyBuffers.cpp', 'owner->input = std::move(input);', 'legacy transactional input publication'),
        @('cmake/ZstdLegacyBuffers.cpp', 'owner->output = std::move(output);', 'legacy transactional output publication'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_legacy_owned_buffers("${source_dir}")', 'legacy owned buffer patch dispatch'),
        @('cmake/ZstdLibrary.cmake', '"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdLegacyBuffers.cpp"', 'legacy production owner linkage'),
        @('tests/cpp/test_zstd_legacy_buffers.cpp', 'TEST_CASE(zstd_legacy_checked_copy_overlap_oracle)', 'legacy independent copy oracle'),
        @('tests/cpp/test_zstd_legacy_buffers.cpp', 'TEST_CASE(zstd_legacy_owned_buffers_allocation_failures)', 'legacy owner failure regression'),
        @('tests/cpp/test_zstd_legacy_failures.cpp', 'TEST_CASE(zstd_legacy_stream_rejects_invalid_public_geometry)', 'legacy public geometry regression'),
        @('cmake/ZstdRawBlockWriter.c', 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize', 'raw-block extent'),
        @('cmake/ZstdCoverSelection.cpp', 'candidateContentSize <= initialized.size()', 'initialized dictionary extent'),
        @('cmake/ZstdCoverSelection.cpp', 'candidateDictSize > initialized.size() / 2', 'dictionary growth overflow'),
        @('cmake/ZstdCoverSelection.cpp', 'content.initializedOffset > content.capacity', 'dictionary allocation geometry'),
        @('cmake/ZstdCoverSelection.cpp', 'content.capacity > static_cast<std::size_t>(PTRDIFF_MAX)', 'dictionary pointer extent'),
        @('cmake/ZstdCoverSelection.cpp', 'using DictionaryOwner = std::unique_ptr<BYTE[], DictionaryDelete>;', 'dictionary scoped ownership'),
        @('cmake/ZstdCoverSelection.cpp', 'sampleOffsets[index] != extent || sampleSizes[index] > limit - extent', 'dictionary packed sample geometry'),
        @('cmake/ZstdCoverSelection.cpp', 'written > output.size() || written > static_cast<std::size_t>(PTRDIFF_MAX) - total', 'dictionary aggregate overflow'),
        @('cmake/ZstdCoverSelection.cpp', 'largestDictSize > dictBufferCapacity', 'dictionary finalized capacity'),
        @('cmake/ZstdDictionaryEvaluation.cmake', 'if (samplesSizes[i] > (size_t)-1 - sum)', 'dictionary input accumulation overflow'),
        @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_dictionary_evaluation("${source_dir}")', 'dictionary evaluation patch dispatch'),
        @('tests/cpp/test_zstd_cover_selection.cpp', 'TEST_CASE(zstd_cover_evaluation_rejects_invalid_sample_geometry)', 'dictionary sample geometry regression'),
        @('tests/cpp/test_zstd_cover_selection.cpp', 'TEST_CASE(zstd_cover_evaluation_matches_independent_readback)', 'dictionary evaluation readback oracle'),
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
        $ownedFragment = "cmake/ZstdLegacyOwnedStreamV$version.c"
        $context = if ($version -eq '05') { 'zbc' } else { 'zbd' }
        $requirements += ,@($ownedFragment, "$context`->inPos > needed || needed > buffers.inputCapacity", "v$version owned input extent")
        $requirements += ,@($ownedFragment, "$context`->outStart > $context`->outEnd || $context`->outEnd > buffers.outputCapacity", "v$version initialized output extent")
        $requirements += ,@($ownedFragment, '*srcSizePtr > (size_t)PTRDIFF_MAX', "v$version public input pointer geometry")
        $requirements += ,@($ownedFragment, 'ZBUFF_viewOwnedBuffers', "v$version actual owner geometry")
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
    Assert-ZstdSourceRequirement -RepoRoot $RepoRoot -Requirements $requirements
    $public = Get-Content -LiteralPath (Join-Path $RepoRoot 'cmake/ZstdLegacyPublicStream.c') -Raw
    $shipped = [regex]::Replace($public, '(?ms)^#if \(ZSTD_LEGACY_SUPPORT <= 4\).*?^#endif\s*', '')
    $shipped = [regex]::Replace($shipped, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
    if ($shipped -match '(input->src|output->dst)\s*=(?!=)') {
        throw 'Zstandard rewrite policy rejects replacement of caller-owned public buffer pointers.'
    }
}
