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
    $pattern = Get-ZstdPolicyTokenPattern -Token $Original
    if (-not [regex]::IsMatch($text, $pattern)) { throw "Mutation fixture drift: $Path/$Original" }
    try {
        $changed = [regex]::Replace($text, $pattern, $Replacement.Replace('$', '$$'))
        [IO.File]::WriteAllText($file, $changed)
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
    # A glob argument must not start a comment spanning later CMake commands;
    # actual CMake and generated-C comments must still fail executable matching.
    $guard = 'canonical_guard()'
    foreach ($case in @(
            @(('file(GLOB sources "${root}/*")' + "`n" + $guard), $true),
            @(('# ' + $guard), $false),
            @(('set(fragment "/* ' + $guard + ' */")'), $false),
            @(('set(fragment [=[/* ' + $guard + ' */]=])'), $false),
            @(('set(fragment [==[/* ' + $guard + ' */]==])'), $false),
            @(('set(fragment "// ' + $guard + '")'), $false),
            @(('set(fragment "#define FLAG 1")' + "`n" + $guard), $true))) {
        $code = Get-ZstdExecutablePolicyText -Text $case[0] -Path 'fixture.cmake'
        if ($code.Contains($guard) -ne $case[1]) { throw 'CMake executable comment boundary regression.' }
    }
    foreach ($relative in @('cmake/ZstdRawBlockWriter.c', 'cmake/ZstdCoverSelection.cpp', 'cmake/ZstdDictionaryBounds.cmake', 'cmake/PatchZstdLegacy.cmake',
            'cmake/ZstdDictionaryEvaluation.cmake',
            'cmake/ZstdHeaderComponents.cmake', 'tests/zstd/headers/header_contract.c',
            'cmake/ZstdDefaultAllocator.cmake', 'tests/zstd/headers/CMakeLists.txt',
            'cmake/ZstdHuffmanTable.c', 'tests/cpp/test_zstd_huffman_table.cpp', 'tests/zstd/fault_allocator.cpp',
            'cmake/ZstdCoverWorkGroup.cpp', 'tests/cpp/test_zstd_work_group.cpp',
            'cmake/ZstdCoverWorkGroup.cmake',
            'cmake/ZstdAlgorithmProgress.cmake', 'cmake/ZstdSuffixRanks.c',
            'tests/cpp/test_zstd_algorithm_progress.cpp', 'tests/cpp/test_zstd_match_progress.cpp',
            'cmake/ZstdFseTableGeometry.c', 'tests/cpp/test_zstd_fse_table.cpp',
            'cmake/ZstdLegacyBuffers.cpp', 'cmake/ZstdLibrary.cmake', 'tests/cpp/test_zstd_legacy_buffers.cpp',
            'cmake/ZstdLegacyOwnedSequence.c', 'cmake/ZstdLegacyOwnedLiterals.c', 'cmake/ZstdLegacyDecoderOwner.c',
            'cmake/ZstdLegacyOwnedDecoder.cmake',
            'cmake/ZstdLegacyOwnedStreamV05.c', 'cmake/ZstdLegacyOwnedStreamV06.c', 'cmake/ZstdLegacyOwnedStreamV07.c',
            'cmake/ZstdLegacyPublicStream.c', 'tests/cpp/test_zstd_bounds.cpp',
            'tests/cpp/test_zstd_cover_selection.cpp', 'tests/cpp/test_zstd_legacy_failures.cpp',
            'cmake/ZstdLegacyLiteralsV05.c', 'cmake/ZstdLegacyLiteralsV06.c', 'cmake/ZstdLegacyLiteralsV07.c',
            'cmake/ZstdLegacyCanonical.cmake', 'tests/cmake/test_zstd_legacy_patch.cmake', 'cmake/ZstdLegacyOwnedBlock.c', 'cmake/ZstdLegacyDictionary.cmake', 'tests/cpp/test_zstd_legacy_history.cpp')) {
        $destination = Join-Path $fixtureRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repoRoot $relative) -Destination $destination
    }
    Assert-ZstdRewritePolicy -RepoRoot $fixtureRoot
    Assert-ZstdPolicyMutation -Path 'cmake/PatchZstdLegacy.cmake' `
        -Original 'superzip_patch_zstd_default_allocator("${source_dir}")' `
        -Replacement '# omitted shared allocator' -Cause 'shared default allocator production dispatch'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdDefaultAllocator.cmake' `
        -Original 'ZSTDLIB_STATIC_API extern ZSTD_customMem const ZSTD_defaultCMem;' `
        -Replacement 'static ZSTD_customMem const ZSTD_defaultCMem;' -Cause 'shared default allocator declaration'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdDefaultAllocator.cmake' `
        -Original 'if(NOT actual STREQUAL _zstd_default_allocator_common_original)' `
        -Replacement 'if(FALSE)' -Cause 'shared default allocator source identity'
    Assert-ZstdPolicyMutation -Path 'tests/zstd/headers/CMakeLists.txt' `
        -Original 'require_default_allocator_sharing(FALSE)' -Replacement '# omitted original control' `
        -Cause 'shared default allocator negative control'
    Assert-ZstdPolicyMutation -Path 'tests/zstd/headers/CMakeLists.txt' `
        -Original 'require_default_allocator_sharing(TRUE)' -Replacement '# omitted production test' `
        -Cause 'shared default allocator production linkage'
    foreach ($mutation in @(
            @('cmake/ZstdLegacyCanonical.cmake', 'if(NOT recognized)', 'if(FALSE)', 'canonical exact source admission'),
            @('cmake/ZstdLegacyCanonical.cmake', 'if(NOT actual STREQUAL "${_zstd_canonical_${version}_patched}")', 'if(FALSE)', 'canonical complete output identity'),
            @('cmake/ZstdLegacyCanonical.cmake', 'if(NOT archive_hash STREQUAL _zstd_canonical_archive_hash)', 'if(FALSE)', 'canonical immutable provenance identity'),
            @('cmake/ZstdLegacyCanonical.cmake', 'superzip_zstd_canonical_geometry("${content}" "${version}" content)', '# omitted geometry', 'canonical complete geometry dispatch'),
            @('cmake/ZstdLegacyCanonical.cmake', 'superzip_zstd_canonical_stream("${content}" "${version}" content)', '# omitted stream', 'legacy owned buffer patch dispatch'),
            @('cmake/ZstdLegacyOwnedSequence.c', 'sequence.matchLength > available - sequence.litLength', 'sequence.matchLength > available', 'canonical sequence extent'),
            @('tests/cmake/test_zstd_legacy_patch.cmake', 'prepare_historical_decoder_stage("${previous_canonical}" "complete")', '', 'canonical historical migration coverage'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_header_components("${source_dir}")', '', 'guarded header component dispatch'),
            @('cmake/ZstdHeaderComponents.cmake', 'if(NOT component_hash STREQUAL "${${key}_${part}_hash}")', 'if(FALSE)', 'complete header component identity'),
            @('cmake/ZstdHeaderComponents.cmake', 'if(EXISTS "${temporary}" OR EXISTS "${raw}")', 'if(FALSE)', 'header interrupted publication rejection'),
            @('tests/zstd/headers/header_contract.c', '#define XXH_INLINE_ALL', '', 'late hash implementation opt-in'),
            @('cmake/ZstdAlgorithmProgress.cmake', 'int prefixSearchComplete = 0;', 'int prefixSearchComplete;', 'match budget and termination separation'),
            @('cmake/ZstdAlgorithmProgress.cmake', '&& !prefixSearchComplete)', ')', 'match completed-prefix dictionary gate'),
            @('cmake/ZstdSuffixRanks.c', 'if (suffix < 0 || suffix >= count)', 'if (suffix < 0)', 'suffix rank index extent'),
            @('cmake/ZstdSuffixRanks.c', 'if (remaining == 0)', 'if (0)', 'suffix group termination'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_algorithm_progress("${source_dir}")', '', 'algorithm progress patch dispatch'),
            @('tests/cpp/test_zstd_algorithm_progress.cpp', 'TEST_CASE(zstd_suffix_progress_matches_lexicographic_oracle)', 'TEST_CASE(removed_suffix_oracle)', 'suffix independent ordering oracle'),
            @('tests/cpp/test_zstd_algorithm_progress.cpp', 'TEST_CASE(zstd_suffix_progress_rejects_malformed_groups)', 'TEST_CASE(removed_suffix_malformed)', 'suffix malformed group regression'),
            @('tests/cpp/test_zstd_match_progress.cpp', 'TEST_CASE(zstd_match_progress_preserves_attached_dictionary_frames)', 'TEST_CASE(removed_match_controls)', 'match pre-rewrite frame controls'),
            @('cmake/ZstdCoverWorkGroup.cpp', 'POOL_joinJobs(pool.get());', '', 'optimizer actual worker completion'),
            @('cmake/ZstdCoverWorkGroup.cmake', '#if PTRDIFF_MAX <= UINT32_MAX', '#if 0', 'sample count architecture range'),
            @('cmake/ZstdCoverWorkGroup.cpp', 'COVER_best_wait(&best);', '', 'optimizer logical completion'),
            @('cmake/ZstdCoverWorkGroup.cpp', 'if (committed && destroyContext != nullptr)', 'if (destroyContext != nullptr)', 'optimizer initialized context cleanup'),
            @('cmake/ZstdCoverWorkGroup.cpp', 'using ContextOwner = std::unique_ptr<std::byte[], ContextDelete>;', 'using ContextOwner = std::byte*;', 'optimizer exclusive context storage'),
            @('cmake/ZstdCoverWorkGroup.cpp', 'group->best.dictSize > capacity', 'group->best.dictSize == 0', 'optimizer output extent'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_work_group("${source_dir}")', '', 'optimizer owner patch dispatch'),
            @('tests/cpp/test_zstd_work_group.cpp', 'TEST_CASE(zstd_work_group_joins_actual_worker_lifetime)', 'TEST_CASE(removed_optimizer_lifetime)', 'optimizer early-completion regression'),
            @('tests/cpp/test_zstd_work_group.cpp', 'TEST_CASE(zstd_work_group_context_transaction_and_failures)', 'TEST_CASE(removed_optimizer_failure)', 'optimizer context failure regression'),
            @('cmake/ZstdLegacyBuffers.cpp', 'using BufferOwner = std::unique_ptr<char[], BufferDelete>;', 'using BufferOwner = char*;', 'legacy exclusive buffer ownership'),
            @('cmake/ZstdLegacyBuffers.cpp', 'count <= capacity - offset', 'count <= capacity', 'legacy complete copy geometry'),
            @('cmake/ZstdLegacyBuffers.cpp', '!valid_extent(destination, destinationCapacity, destinationOffset, count)', '!valid_extent(destination, destinationCapacity, 0, count)', 'legacy copy destination geometry'),
            @('cmake/ZstdLegacyBuffers.cpp', '!valid_extent(source, sourceCapacity, sourceOffset, count)', '!valid_extent(source, sourceCapacity, 0, count)', 'legacy copy source geometry'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_canonical("${source_dir}")', '', 'legacy complete decoder ownership dispatch'),
            @('cmake/ZstdLegacyBuffers.cpp', 'count > distance ||', 'count > capacity ||', 'legacy initialized history transfer extent'),
            @('cmake/ZstdLegacyBuffers.cpp', 'distance > owner->initialized', 'distance > owner->capacity', 'legacy initialized history distance'),
            @('cmake/ZstdLegacyBuffers.cpp', 'owner->initialized = std::min(owner->initialized, limit);', 'owner->initialized = owner->capacity;', 'legacy decoded window retention bound'),
            @('cmake/ZstdLegacyOwnedSequence.c', 'dictionaryOffset > ZBUFF_ownedHistorySize(history)', 'dictionaryOffset > 0', 'legacy actual sequence history extent'),
            @('cmake/ZstdLegacyOwnedLiterals.c', 'litSize > sizeof(dctx->litBuffer) - WILDCOPY_OVERLENGTH', 'litSize > sizeof(dctx->litBuffer)', 'legacy raw literal padding extent'),
            @('cmake/ZstdLegacyOwnedLiterals.c', 'dctx->litPtr = dctx->litBuffer;', 'dctx->litPtr = istart;', 'legacy independent raw literal lifetime'),
            @('cmake/ZstdLegacyDecoderOwner.c', 'destination->owner = owner;', '', 'legacy cloned destination ownership identity'),
            @('cmake/ZstdLegacyDecoderOwner.c', 'destination->customMem = allocator;', '', 'legacy cloned custom allocator identity'),
            @('cmake/ZstdLegacyOwnedDecoder.cmake', 'if (dctx->historyError) return dctx->historyError;', '', 'legacy prepared clone error propagation'),
            @('tests/cpp/test_zstd_legacy_history.cpp', 'TEST_CASE(zstd_legacy_prepared_history_and_literals_outlive_sources)', 'TEST_CASE(removed_source_expiry)', 'legacy prepared source expiry regression'),
            @('tests/cpp/test_zstd_legacy_history.cpp', 'TEST_CASE(zstd_legacy_prepared_clone_failure_blocks_frame_output)', 'TEST_CASE(removed_clone_output)', 'legacy prepared clone output regression'),
            @('cmake/ZstdLegacyBuffers.cpp', 'std::copy_backward(input.begin(), input.end(), output.end());', 'std::copy(input.begin(), input.end(), output.begin());', 'legacy backward overlap transfer'),
            @('cmake/ZstdLegacyBuffers.cpp', 'if (input.data() == output.data())', 'if (0)', 'legacy identical-range copy contract'),
            @('tests/cpp/test_zstd_legacy_buffers.cpp', 'TEST_CASE(zstd_legacy_checked_copy_overlap_oracle)', 'TEST_CASE(removed_copy_oracle)', 'legacy independent copy oracle'),
            @('cmake/ZstdRawBlockWriter.c', 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize', 'srcSize + ZSTD_blockHeaderSize > dstCapacity', 'raw-block extent'),
            @('cmake/ZstdCoverSelection.cpp', 'candidateContentSize <= initialized.size()', 'candidateContentSize <= largestDictSize', 'initialized dictionary extent'),
            @('cmake/ZstdCoverSelection.cpp', 'candidateDictSize > initialized.size() / 2', 'candidateDictSize * 2 > initialized.size()', 'dictionary growth overflow'),
            @('cmake/ZstdCoverSelection.cpp', 'content.initializedOffset > content.capacity', 'content.initializedOffset > 0', 'dictionary allocation geometry'),
            @('cmake/ZstdCoverSelection.cpp', 'content.capacity > static_cast<std::size_t>(PTRDIFF_MAX)', 'content.capacity > 0', 'dictionary pointer extent'),
            @('cmake/ZstdCoverSelection.cpp', 'using DictionaryOwner = std::unique_ptr<BYTE[], DictionaryDelete>;', 'using DictionaryOwner = BYTE*;', 'dictionary scoped ownership'),
            @('cmake/ZstdHuffmanTable.c', 'count > (table.capacity - entries) / length', 'count > table.capacity', 'Huffman ranked table capacity'),
            @('cmake/ZstdHuffmanTable.c', 'symbol != symbolCount', 'symbol > symbolCount', 'Huffman complete symbol geometry'),
            @('cmake/ZstdFseTableGeometry.c', 'tableLog > 15 || maxSymbolValue > 255', 'tableLog > 15', 'FSE descriptor geometry'),
            @('cmake/ZstdCoverSelection.cpp', 'sampleOffsets[index] != extent || sampleSizes[index] > limit - extent', 'sampleOffsets[index] != extent', 'dictionary packed sample geometry'),
            @('cmake/ZstdCoverSelection.cpp', 'written > output.size() || written > static_cast<std::size_t>(PTRDIFF_MAX) - total', 'written > output.size()', 'dictionary aggregate overflow'),
            @('cmake/ZstdCoverSelection.cpp', 'largestDictSize > dictBufferCapacity', 'largestDictSize == 0', 'dictionary finalized capacity'),
            @('cmake/ZstdDictionaryEvaluation.cmake', 'if (samplesSizes[i] > (size_t)-1 - sum)', 'if (0)', 'dictionary input accumulation overflow'),
            @('cmake/ZstdCoverSelection.cpp', 'const auto suffix = initialized.last(candidateContentSize);', 'const auto suffix = allocation.last(candidateContentSize);', 'dictionary bounded suffix'),
            @('cmake/ZstdDictionaryBounds.cmake', 'COVER_freeSelectedDictionary(selection.dictContent);', 'free(selection.dictContent);', 'dictionary matching release'),
            @('cmake/PatchZstdLegacy.cmake', 'superzip_patch_zstd_raw_block_writer("${source_dir}")', '# omitted raw-block patch', 'raw-block patch dispatch'),
            @('cmake/ZstdLegacyPublicStream.c', '    switch(version)', "    output->dst = legacyContext;`n    switch(version)", 'caller-owned public buffer'),
            @('tests/cpp/test_zstd_bounds.cpp', 'TEST_CASE(zstd_raw_block_writer_overflow_rejection)', 'TEST_CASE(removed_overflow)', 'raw-block overflow regression'))) {
        Assert-ZstdPolicyMutation -Path $mutation[0] -Original $mutation[1] -Replacement $mutation[2] -Cause $mutation[3]
    }
    foreach ($version in @('05', '06', '07')) {
        $owned = "cmake/ZstdLegacyOwnedStreamV$version.c"
        $context = if ($version -eq '05') { 'zbc' } else { 'zbd' }
        Assert-ZstdPolicyMutation -Path $owned -Original "$context`->inPos > needed || needed > buffers.inputCapacity" `
            -Replacement "$context`->inPos > needed" -Cause "v$version owned input extent"
        Assert-ZstdPolicyMutation -Path $owned -Original '*srcSizePtr > (size_t)PTRDIFF_MAX' `
            -Replacement '*srcSizePtr > SIZE_MAX' -Cause "v$version public input pointer geometry"
        Assert-ZstdPolicyMutation -Path $owned -Original "$context`->outStart > $context`->outEnd || $context`->outEnd > buffers.outputCapacity" `
            -Replacement "$context`->outStart > $context`->outEnd" -Cause "v$version initialized output extent"
        Assert-ZstdPolicyMutation -Path $owned -Original '*srcSizePtr != 0 ? istart + *srcSizePtr : istart' `
            -Replacement 'istart + *srcSizePtr' -Cause "v$version nullable input extent"
        $literal = "cmake/ZstdLegacyLiteralsV$version.c"
        Assert-ZstdPolicyMutation -Path $literal -Original "return ZSTDv$version`_decodeRawLiterals(dctx, istart, srcSize);" `
            -Replacement 'return 0;' -Cause "v$version canonical raw literal routing"
    }
    # A comment containing the removed check cannot satisfy the executable-source guard.
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdRawBlockWriter.c' `
        -Original 'dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize' `
        -Replacement '/* dstCapacity < ZSTD_blockHeaderSize || srcSize > dstCapacity - ZSTD_blockHeaderSize */ 0' `
        -Cause 'raw-block extent'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyOwnedBlock.c' -Original 'if (ZSTDvXX_isError(result))' `
        -Replacement 'if (0)' -Cause 'legacy error history boundary'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyCanonical.cmake' `
        -Original 'superzip_zstd_owned_decoder_components("${content}" "${version}" content)' `
        -Replacement '# omitted legacy history patch' -Cause 'legacy history patch dispatch'
    Assert-ZstdPolicyMutation -Path 'cmake/ZstdLegacyCanonical.cmake' `
        -Original 'superzip_zstd_rewrite_legacy_dictionary("${content}" "${version}" content)' `
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
            @('superzip_patch_zstd_dictionary_bounds', 'dictionary bounds patch dispatch'))) {
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
