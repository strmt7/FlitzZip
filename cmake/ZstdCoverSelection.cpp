#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>

extern "C" {
#include "dictBuilder/cover.h"
#include "common/error_private.h"
#include "zstd.h"
}

#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
#include "fault_allocator.h"
#endif

namespace {
struct DictionaryDelete {
    // Purpose: Release a selected dictionary with its matching allocator.
    // Inputs: A transferred array or null. Outputs: Releases exactly one owner; never throws.
    void operator()(BYTE* data) const noexcept {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
        sz_fault_free(data);
#else
        delete[] data;
#endif
    }
};
using DictionaryOwner = std::unique_ptr<BYTE[], DictionaryDelete>;

// Purpose: Acquire a move-only dictionary array, with serial allocator interposition in the test build.
// Inputs: Validated byte capacity. Outputs: One owner or an empty owner on allocation failure.
DictionaryOwner allocate_dictionary(std::size_t capacity) noexcept {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
    return DictionaryOwner(static_cast<BYTE*>(sz_fault_malloc(capacity)));
#else
    return DictionaryOwner(new (std::nothrow) BYTE[capacity]);
#endif
}

// Purpose: Transfer exactly one completed dictionary to the private C consumer.
// Inputs: An exclusive owner and validated finalized size/compression result.
// Outputs: Returns the C record; caller must use COVER_freeSelectedDictionary to release it.
COVER_dictSelection_t transfer_dictionary(DictionaryOwner owner, std::size_t size, std::size_t compressed) noexcept {
    return {owner.release(), size, compressed};
}
}  // namespace

// Purpose: Release a transferred selector array from the private C boundary.
// Inputs: An array returned by COVER_selectDictOwned or null. Outputs: Matching release, with no exception.
extern "C" void COVER_freeSelectedDictionary(BYTE* data) {
    DictionaryDelete{}(data);
}

// Purpose: Select a dictionary with checked allocation geometry and scoped ownership.
// Inputs: Complete initialized input allocation, independent output capacity and live sample extents.
// Outputs: Transfers one successful array or returns an error; every other owner is released automatically.
extern "C" COVER_dictSelection_t COVER_selectDictOwned(COVER_dictContent_t content, std::size_t dictBufferCapacity,
                                                       const BYTE* samplesBuffer, const std::size_t* samplesSizes,
                                                       unsigned nbFinalizeSamples, std::size_t nbCheckSamples,
                                                       std::size_t nbSamples, ZDICT_cover_params_t params,
                                                       std::size_t* offsets, std::size_t totalCompressedSize) {
    if (content.data == nullptr || content.initializedOffset > content.capacity ||
        content.capacity > static_cast<std::size_t>(PTRDIFF_MAX) ||
        dictBufferCapacity > static_cast<std::size_t>(PTRDIFF_MAX)) {
        return COVER_dictSelectionError(ERROR(parameter_outOfBound));
    }
    const std::span<const BYTE> allocation(content.data, content.capacity);
    const auto initialized = allocation.subspan(content.initializedOffset);
    auto largest = allocate_dictionary(dictBufferCapacity);
    auto candidate = allocate_dictionary(dictBufferCapacity);
    if (!largest || !candidate) {
        return COVER_dictSelectionError(ERROR(memory_allocation));
    }
    const auto largestDictSize =
        ZDICT_finalizeDictionary(largest.get(), dictBufferCapacity, initialized.data(), initialized.size(),
                                 samplesBuffer, samplesSizes, nbFinalizeSamples, params.zParams);
    if (ZDICT_isError(largestDictSize)) {
        return COVER_dictSelectionError(largestDictSize);
    }
    const auto largestCompressedSize = COVER_checkTotalCompressedSize(
        params, samplesSizes, samplesBuffer, offsets, nbCheckSamples, nbSamples, largest.get(), largestDictSize);
    if (ZSTD_isError(largestCompressedSize)) {
        return COVER_dictSelectionError(largestCompressedSize);
    }
    if (params.shrinkDict == 0) {
        return transfer_dictionary(std::move(largest), largestDictSize, largestCompressedSize);
    }
    const auto regressionTolerance = static_cast<double>(params.shrinkDictMaxRegression) / 100.0 + 1.00;
    std::size_t candidateContentSize = ZDICT_DICTSIZE_MIN;
    while (candidateContentSize < largestDictSize && candidateContentSize <= initialized.size()) {
        const auto suffix = initialized.last(candidateContentSize);
        const auto candidateDictSize =
            ZDICT_finalizeDictionary(candidate.get(), dictBufferCapacity, suffix.data(), suffix.size(), samplesBuffer,
                                     samplesSizes, nbFinalizeSamples, params.zParams);
        if (ZDICT_isError(candidateDictSize)) {
            return COVER_dictSelectionError(candidateDictSize);
        }
        totalCompressedSize =
            COVER_checkTotalCompressedSize(params, samplesSizes, samplesBuffer, offsets, nbCheckSamples, nbSamples,
                                           candidate.get(), candidateDictSize);
        if (ZSTD_isError(totalCompressedSize)) {
            return COVER_dictSelectionError(totalCompressedSize);
        }
        if (static_cast<double>(totalCompressedSize) <=
            static_cast<double>(largestCompressedSize) * regressionTolerance) {
            return transfer_dictionary(std::move(candidate), candidateDictSize, totalCompressedSize);
        }
        if (candidateDictSize > initialized.size() / 2) {
            break;
        }
        candidateContentSize = candidateDictSize * 2;
    }
    return transfer_dictionary(std::move(largest), largestDictSize, largestCompressedSize);
}
