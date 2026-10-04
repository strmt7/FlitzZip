#include <algorithm>
#include <cmath>
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

struct SampleViews {
    std::span<const BYTE> bytes;
    std::span<const std::size_t> sizes;
    std::span<const std::size_t> offsets;
    std::size_t first = 0;
};

// Purpose: Validate packed sample metadata before creating any byte subview.
// Inputs: Live caller-owned arrays of count elements and their complete packed sample allocation.
// Outputs: Captures checked geometry or returns false without allocating or reading sample bytes.
bool capture_samples(const BYTE* bytes, const std::size_t* sizes, const std::size_t* offsets, std::size_t trainCount,
                     std::size_t count, double splitPoint, SampleViews& result) noexcept {
    constexpr auto limit = static_cast<std::size_t>(PTRDIFF_MAX);
    if (count > limit / sizeof(std::size_t) || trainCount > count || !std::isfinite(splitPoint) || splitPoint < 0.0 ||
        splitPoint > 1.0 || (count != 0 && (sizes == nullptr || offsets == nullptr))) {
        return false;
    }
    const std::span<const std::size_t> sampleSizes(sizes, count);
    const std::span<const std::size_t> sampleOffsets(offsets, count);
    std::size_t extent = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (sampleOffsets[index] != extent || sampleSizes[index] > limit - extent) {
            return false;
        }
        extent += sampleSizes[index];
    }
    if (bytes == nullptr && extent != 0) {
        return false;
    }
    result = {std::span<const BYTE>(bytes, extent), sampleSizes, sampleOffsets, splitPoint < 1.0 ? trainCount : 0};
    return true;
}

// Purpose: Acquire a move-only dictionary array, with serial allocator interposition in the test build.
// Inputs: Validated byte capacity. Outputs: One owner or an empty owner on allocation failure.
DictionaryOwner allocate_dictionary(std::size_t capacity) noexcept {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
    return DictionaryOwner(static_cast<BYTE*>(sz_fault_malloc(capacity)));
#else
    return DictionaryOwner(new (std::nothrow) BYTE[capacity]);
#endif
}

// Purpose: Evaluate a candidate with bounded sample/output views and scoped compression resources.
// Inputs: Validated sample metadata, a live initialized dictionary and the requested compression level.
// Outputs: Returns the complete size or a Zstandard error; every temporary owner is released on all exits.
std::size_t evaluate_dictionary(const SampleViews& samples, std::span<const BYTE> dictionary, int level) noexcept {
    std::size_t maximum = 0;
    for (const auto size : samples.sizes.subspan(samples.first)) {
        maximum = (std::max)(maximum, size);
    }
    const auto capacity = ZSTD_compressBound(maximum);
    if (ZSTD_isError(capacity) || capacity > static_cast<std::size_t>(PTRDIFF_MAX)) {
        return ERROR(parameter_outOfBound);
    }
    auto destination = allocate_dictionary(capacity);
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), ZSTD_freeCCtx);
    const std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> prepared(
        ZSTD_createCDict(dictionary.data(), dictionary.size(), level), ZSTD_freeCDict);
    if (!destination || !context || !prepared) {
        return ERROR(memory_allocation);
    }
    const std::span<BYTE> output(destination.get(), capacity);
    auto total = dictionary.size();
    for (auto index = samples.first; index < samples.sizes.size(); ++index) {
        const auto input = samples.bytes.subspan(samples.offsets[index], samples.sizes[index]);
        const auto written = ZSTD_compress_usingCDict(context.get(), output.data(), output.size(), input.data(),
                                                      input.size(), prepared.get());
        if (ZSTD_isError(written)) {
            return written;
        }
        if (written > output.size() || written > static_cast<std::size_t>(PTRDIFF_MAX) - total) {
            return ERROR(parameter_outOfBound);
        }
        total += written;
    }
    return total;
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

// Purpose: Validate the private C evaluation boundary before entering the bounded compression implementation.
// Inputs: Live packed sample arrays/bytes and dictionary allocation; declared extents remain caller obligations.
// Outputs: Returns a compressed-size measurement or an ordinary parameter/allocation/compression error.
extern "C" std::size_t COVER_checkTotalCompressedSize(ZDICT_cover_params_t parameters, const std::size_t* sizes,
                                                      const BYTE* bytes, std::size_t* offsets, std::size_t trainCount,
                                                      std::size_t count, BYTE* dictionary, std::size_t dictionarySize) {
    SampleViews samples;
    if ((dictionary == nullptr && dictionarySize != 0) || dictionarySize > static_cast<std::size_t>(PTRDIFF_MAX) ||
        !capture_samples(bytes, sizes, offsets, trainCount, count, parameters.splitPoint, samples)) {
        return ERROR(parameter_outOfBound);
    }
    return evaluate_dictionary(samples, std::span<const BYTE>(dictionary, dictionarySize),
                               parameters.zParams.compressionLevel);
}

// Purpose: Select a dictionary with checked allocation geometry and scoped ownership.
// Inputs: Complete initialized input allocation, independent output capacity and live sample extents.
// Outputs: Transfers one successful array or returns an error; every other owner is released automatically.
extern "C" COVER_dictSelection_t COVER_selectDictOwned(COVER_dictContent_t content, std::size_t dictBufferCapacity,
                                                       const BYTE* samplesBuffer, const std::size_t* samplesSizes,
                                                       unsigned nbFinalizeSamples, std::size_t nbCheckSamples,
                                                       std::size_t nbSamples, ZDICT_cover_params_t params,
                                                       std::size_t* offsets, std::size_t totalCompressedSize) {
    SampleViews samples;
    if (content.data == nullptr || content.initializedOffset > content.capacity ||
        content.capacity > static_cast<std::size_t>(PTRDIFF_MAX) ||
        dictBufferCapacity > static_cast<std::size_t>(PTRDIFF_MAX) || nbFinalizeSamples > nbSamples ||
        !capture_samples(samplesBuffer, samplesSizes, offsets, nbCheckSamples, nbSamples, params.splitPoint, samples)) {
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
    if (largestDictSize > dictBufferCapacity) {
        return COVER_dictSelectionError(ERROR(parameter_outOfBound));
    }
    const auto largestCompressedSize =
        evaluate_dictionary(samples, std::span<const BYTE>(largest.get(), dictBufferCapacity).first(largestDictSize),
                            params.zParams.compressionLevel);
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
        if (candidateDictSize > dictBufferCapacity) {
            return COVER_dictSelectionError(ERROR(parameter_outOfBound));
        }
        totalCompressedSize = evaluate_dictionary(
            samples, std::span<const BYTE>(candidate.get(), dictBufferCapacity).first(candidateDictSize),
            params.zParams.compressionLevel);
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
