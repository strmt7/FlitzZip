#include "test_util.hpp"

#define ZDICT_STATIC_LINKING_ONLY
extern "C" {
#include "cover.h"
#include "fault_allocator.h"
}
#include "zstd.h"
#include "zstd_errors.h"

#include <array>
#include <limits>
#include <memory>
#include <span>

namespace {

struct FinalizerProbe {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::size_t calls = 0;
    std::size_t fail_on = 0;
    bool invalid_extent = false;
};

FinalizerProbe probe;

struct SelectionDeleter {
    // Purpose: Release the owned selection through the production helper.
    // Inputs: selection is null or one complete/error selection owned by the test.
    // Outputs: Releases tracked dictionary storage and the fixture wrapper without throwing.
    void operator()(COVER_dictSelection_t* selection) const noexcept {
        if (selection != nullptr) {
            COVER_dictSelectionFree(*selection);
            delete selection;
        }
    }
};

using SelectionOwner = std::unique_ptr<COVER_dictSelection_t, SelectionDeleter>;

struct SelectionFixture {
    std::vector<BYTE> content;
    std::vector<BYTE> samples;
    std::array<std::size_t, 128> sizes{};
    std::array<std::size_t, 128> offsets{};

    // Purpose: Build bounded deterministic content and samples reproducing dictionary shrinking.
    // Inputs: content_size is positive; seed chooses a reproducible low-alphabet byte sequence.
    // Outputs: Owns exact input extents and 128 independent 512-byte samples.
    SelectionFixture(std::size_t content_size, std::uint32_t seed) : content(content_size) {
        REQUIRE_TRUE(content_size != 0U);
        for (auto& value : content) {
            seed = seed * 1664525U + 1013904223U;
            value = static_cast<BYTE>((seed >> 24U) & 31U);
        }
        samples.resize(sizes.size() * 512U);
        for (std::size_t row = 0; row < sizes.size(); ++row) {
            sizes[row] = 512U;
            offsets[row] = row * 512U;
            for (std::size_t byte = 0; byte < sizes[row]; ++byte) {
                samples[offsets[row] + byte] = content[(row * 37U + byte) % content.size()];
            }
        }
    }

    // Purpose: Exercise the exact generated selection helper with observed finalizer inputs.
    // Inputs: shrink and regression configure production selection; fail_on injects a finalizer error.
    // Outputs: Returns an owned result; probe records any input extent violation before a read.
    SelectionOwner select(bool shrink, unsigned regression = 0U, std::size_t fail_on = 0U) {
        probe = {reinterpret_cast<std::uintptr_t>(content.data()),
                 reinterpret_cast<std::uintptr_t>(content.data()) + content.size(), 0U, fail_on, false};
        ZDICT_cover_params_t params{};
        params.splitPoint = 1.0;
        params.shrinkDict = shrink ? 1U : 0U;
        params.shrinkDictMaxRegression = regression;
        params.zParams.compressionLevel = 3;
        const auto count = static_cast<unsigned>(sizes.size());
        auto result = std::make_unique<COVER_dictSelection_t>();
        const COVER_dictContent_t input{content.data(), content.size(), 0U};
        *result = COVER_selectDict(input, 4096U, samples.data(), sizes.data(), count, count, count, params,
                                   offsets.data(), 0U);
        return SelectionOwner(result.release());
    }
};

// Purpose: Require successful selection and untouched input extent/ownership contracts.
// Inputs: selection owns a result from the current serial fixture.
// Outputs: Throws on error, invalid finalizer input or an oversized finalized dictionary.
void require_selection(const SelectionOwner& selection) {
    REQUIRE_TRUE(!probe.invalid_extent);
    REQUIRE_TRUE(!COVER_dictSelectionIsError(*selection));
    REQUIRE_TRUE(selection->dictSize <= 4096U);
    REQUIRE_TRUE(selection->dictSize != 0U);
}

// Purpose: Verify selected dictionaries with real product compression and decompression.
// Inputs: selection is successful; fixture owns a complete source sample and dictionary input bytes.
// Outputs: Requires exact sample read-back with the selected dictionary and normal context cleanup.
void require_readback(const SelectionOwner& selection, const SelectionFixture& fixture) {
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressor(ZSTD_createCCtx(), ZSTD_freeCCtx);
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressor(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(compressor != nullptr && decompressor != nullptr);
    const auto size = fixture.sizes.front();
    std::vector<BYTE> compressed(ZSTD_compressBound(size));
    std::vector<BYTE> restored(size);
    const auto written =
        ZSTD_compress_usingDict(compressor.get(), compressed.data(), compressed.size(), fixture.samples.data(), size,
                                selection->dictContent, selection->dictSize, 3);
    REQUIRE_TRUE(!ZSTD_isError(written));
    REQUIRE_EQ(ZSTD_decompress_usingDict(decompressor.get(), restored.data(), restored.size(), compressed.data(),
                                         written, selection->dictContent, selection->dictSize),
               size);
    REQUIRE_TRUE(std::ranges::equal(restored, std::span(fixture.samples).first(size)));
}

}  // namespace

// Purpose: Validate every real finalizer input before forwarding to the product DLL.
// Inputs: Production arguments plus the serial probe's exact initialized interval.
// Outputs: Returns the real finalizer result or an injected/extent error without reading invalid bytes.
extern "C" std::size_t sz_checked_finalize(void* destination, std::size_t capacity, const void* content,
                                           std::size_t size, const void* samples, const std::size_t* sizes,
                                           unsigned count, ZDICT_params_t params) {
    ++probe.calls;
    const auto address = reinterpret_cast<std::uintptr_t>(content);
    if (address < probe.begin || address > probe.end || size > probe.end - address) {
        probe.invalid_extent = true;
        return 0U - static_cast<std::size_t>(ZSTD_error_GENERIC);
    }
    if (probe.calls == probe.fail_on) {
        return 0U - static_cast<std::size_t>(ZSTD_error_dictionaryCreation_failed);
    }
    return ZDICT_finalizeDictionary(destination, capacity, content, size, samples, sizes, count, params);
}

// Purpose: Reproduce the original 808-byte request from 680 initialized bytes and prove fallback identity.
// Inputs: Deterministic seed-one corpus with shrinking enabled and zero tolerated regression.
// Outputs: Requires valid suffixes and the byte-identical full dictionary fallback.
TEST_CASE(zstd_cover_shrinking_preserves_initialized_extent) {
    REQUIRE_TRUE(sz_fault_reset(0U));
    SelectionFixture fixture(680U, 1U);
    const auto full = fixture.select(false);
    require_selection(full);
    const auto shrunk = fixture.select(true);
    require_selection(shrunk);
    REQUIRE_TRUE(probe.calls > 1U);
    REQUIRE_EQ(shrunk->dictSize, full->dictSize);
    REQUIRE_EQ(shrunk->totalCompressedSize, full->totalCompressedSize);
    REQUIRE_TRUE(std::ranges::equal(std::span(shrunk->dictContent, shrunk->dictSize),
                                    std::span(full->dictContent, full->dictSize)));
}

// Purpose: Cover candidate/input boundaries and successful smaller dictionary selection using real finalization.
// Inputs: Three seeded corpora and exact content sizes around initial and grown candidate boundaries.
// Outputs: Requires bounded successful dictionaries, preserved inputs and no tracked allocations after cleanup.
TEST_CASE(zstd_cover_shrinking_boundary_candidates) {
    constexpr std::array<std::size_t, 12> lengths{128U, 255U, 256U, 257U,  512U,  679U,
                                                  680U, 681U, 808U, 1024U, 1599U, 1600U};
    std::size_t smaller_dictionaries = 0U;
    for (const auto seed : {1U, 2U, 3U}) {
        for (const auto length : lengths) {
            REQUIRE_TRUE(sz_fault_reset(0U));
            SelectionFixture fixture(length, seed);
            const auto before = fixture.content;
            {
                const auto full = fixture.select(false);
                require_selection(full);
                const auto result = fixture.select(true, 100U);
                require_selection(result);
                if (result->dictSize < full->dictSize) {
                    ++smaller_dictionaries;
                }
                require_readback(result, fixture);
                REQUIRE_EQ(before, fixture.content);
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
    }
    REQUIRE_TRUE(smaller_dictionaries > 0U);
}

// Purpose: Verify complete cleanup for failed allocation and either finalizer call.
// Inputs: Failures at both selection buffers and full/candidate finalization.
// Outputs: Requires proper error codes, no invalid suffix access and no leaked/double-freed storage.
TEST_CASE(zstd_cover_selection_failure_ownership) {
    SelectionFixture fixture(680U, 1U);
    for (const auto allocation : {1U, 2U, 3U}) {
        REQUIRE_TRUE(sz_fault_reset(allocation));
        {
            const auto result = fixture.select(true);
            REQUIRE_TRUE(COVER_dictSelectionIsError(*result));
            REQUIRE_TRUE(ZDICT_isError(result->totalCompressedSize));
            REQUIRE_TRUE(!probe.invalid_extent);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
    for (const auto finalizer : {1U, 2U}) {
        REQUIRE_TRUE(sz_fault_reset(0U));
        {
            const auto result = fixture.select(true, 0U, finalizer);
            REQUIRE_TRUE(COVER_dictSelectionIsError(*result));
            REQUIRE_TRUE(ZDICT_isError(result->totalCompressedSize));
            REQUIRE_TRUE(!probe.invalid_extent);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Reject invalid allocation geometry before any finalizer access or allocation.
// Inputs: A live fixture plus null, reversed-offset and unrepresentable source/output extents.
// Outputs: Requires ordinary parameter errors, no finalizer invocation and no retained allocation.
TEST_CASE(zstd_cover_selection_rejects_invalid_allocation_geometry) {
    SelectionFixture fixture(680U, 1U);
    const auto excessive = static_cast<std::size_t>(PTRDIFF_MAX) + 1U;
    const std::array<COVER_dictContent_t, 4> invalid{{
        {nullptr, 0U, 0U},
        {fixture.content.data(), fixture.content.size(), fixture.content.size() + 1U},
        {fixture.content.data(), excessive, 0U},
        {fixture.content.data(), fixture.content.size(), excessive},
    }};
    ZDICT_cover_params_t params{};
    for (const auto input : invalid) {
        REQUIRE_TRUE(sz_fault_reset(1U));
        probe = {};
        const auto result = COVER_selectDict(input, 4096U, fixture.samples.data(), fixture.sizes.data(), 128U, 128U,
                                             128U, params, fixture.offsets.data(), 0U);
        REQUIRE_TRUE(COVER_dictSelectionIsError(result));
        REQUIRE_EQ(ZSTD_getErrorCode(result.totalCompressedSize), ZSTD_error_parameter_outOfBound);
        REQUIRE_EQ(probe.calls, 0U);
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    }
    REQUIRE_TRUE(sz_fault_reset(1U));
    const COVER_dictContent_t valid{fixture.content.data(), fixture.content.size(), 0U};
    const auto result = COVER_selectDict(valid, excessive, fixture.samples.data(), fixture.sizes.data(), 128U, 128U,
                                         128U, params, fixture.offsets.data(), 0U);
    REQUIRE_TRUE(COVER_dictSelectionIsError(result));
    REQUIRE_EQ(ZSTD_getErrorCode(result.totalCompressedSize), ZSTD_error_parameter_outOfBound);
    REQUIRE_EQ(probe.calls, 0U);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
}

// Purpose: Reject malformed packed sample metadata before finalization or temporary allocation.
// Inputs: Live small arrays with gaps, reversed offsets, overflow, invalid counts and null byte storage.
// Outputs: Requires parameter errors without sample reads, finalizer calls or retained allocations.
TEST_CASE(zstd_cover_evaluation_rejects_invalid_sample_geometry) {
    SelectionFixture fixture(680U, 1U);
    ZDICT_cover_params_t params{};
    params.splitPoint = 1.0;
    const auto reject = [&](const BYTE* bytes, const std::size_t* sizes, std::size_t* offsets, std::size_t train,
                            std::size_t count, double split) {
        REQUIRE_TRUE(sz_fault_reset(1U));
        params.splitPoint = split;
        const auto result = COVER_checkTotalCompressedSize(params, sizes, bytes, offsets, train, count,
                                                           fixture.content.data(), fixture.content.size());
        REQUIRE_EQ(ZSTD_getErrorCode(result), ZSTD_error_parameter_outOfBound);
        REQUIRE_EQ(sz_fault_allocation_attempts(), 0U);
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    };
    std::array<std::size_t, 2> sizes{8U, 8U};
    std::array<std::size_t, 2> offsets{0U, 9U};
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, 2U, 1.0);
    offsets = {1U, 8U};
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, 2U, 1.0);
    offsets = {0U, 8U};
    sizes[1] = static_cast<std::size_t>(PTRDIFF_MAX);
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, 2U, 1.0);
    sizes[1] = 8U;
    reject(nullptr, sizes.data(), offsets.data(), 0U, 2U, 1.0);
    reject(fixture.samples.data(), nullptr, offsets.data(), 0U, 2U, 1.0);
    reject(fixture.samples.data(), sizes.data(), nullptr, 0U, 2U, 1.0);
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 3U, 2U, 1.0);
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, 2U, -1.0);
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, 2U, std::numeric_limits<double>::quiet_NaN());
    reject(fixture.samples.data(), sizes.data(), offsets.data(), 0U, static_cast<std::size_t>(PTRDIFF_MAX), 1.0);
    REQUIRE_TRUE(sz_fault_reset(1U));
    probe = {};
    params.splitPoint = 1.0;
    offsets[1] = 9U;
    const COVER_dictContent_t content{fixture.content.data(), fixture.content.size(), 0U};
    const auto selected =
        COVER_selectDict(content, 4096U, fixture.samples.data(), sizes.data(), 2U, 0U, 2U, params, offsets.data(), 0U);
    REQUIRE_EQ(ZSTD_getErrorCode(selected.totalCompressedSize), ZSTD_error_parameter_outOfBound);
    REQUIRE_EQ(probe.calls, 0U);
    REQUIRE_EQ(sz_fault_allocation_attempts(), 0U);
}

// Purpose: Compare candidate evaluation with independent frame read-back and an externally accumulated total.
// Inputs: A real finalized dictionary and training/full-corpus splits over the deterministic fixture.
// Outputs: Requires exact aggregate sizes, unchanged samples and dictionaries, and complete temporary cleanup.
TEST_CASE(zstd_cover_evaluation_matches_independent_readback) {
    REQUIRE_TRUE(sz_fault_reset(0U));
    SelectionFixture fixture(680U, 1U);
    const auto selection = fixture.select(false);
    require_selection(selection);
    const auto originalSamples = fixture.samples;
    const std::vector<BYTE> originalDictionary(selection->dictContent, selection->dictContent + selection->dictSize);
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressor(ZSTD_createCCtx(), ZSTD_freeCCtx);
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressor(ZSTD_createDCtx(), ZSTD_freeDCtx);
    const std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> dictionary(
        ZSTD_createCDict(selection->dictContent, selection->dictSize, 3), ZSTD_freeCDict);
    REQUIRE_TRUE(compressor && decompressor && dictionary);
    for (const auto first : {0U, 1U, 127U, 128U}) {
        std::size_t expected = selection->dictSize;
        for (std::size_t index = first; index < fixture.sizes.size(); ++index) {
            const auto sample = std::span(fixture.samples).subspan(fixture.offsets[index], fixture.sizes[index]);
            std::vector<BYTE> compressed(ZSTD_compressBound(sample.size()));
            std::vector<BYTE> restored(sample.size());
            const auto written = ZSTD_compress_usingCDict(compressor.get(), compressed.data(), compressed.size(),
                                                          sample.data(), sample.size(), dictionary.get());
            REQUIRE_TRUE(!ZSTD_isError(written));
            REQUIRE_EQ(ZSTD_decompress_usingDict(decompressor.get(), restored.data(), restored.size(),
                                                 compressed.data(), written, selection->dictContent,
                                                 selection->dictSize),
                       sample.size());
            REQUIRE_TRUE(std::ranges::equal(sample, restored));
            expected += written;
        }
        ZDICT_cover_params_t params{};
        params.splitPoint = first == 0U ? 1.0 : 0.5;
        params.zParams.compressionLevel = 3;
        const auto live = sz_fault_live_allocations();
        const auto measured =
            COVER_checkTotalCompressedSize(params, fixture.sizes.data(), fixture.samples.data(), fixture.offsets.data(),
                                           first, fixture.sizes.size(), selection->dictContent, selection->dictSize);
        REQUIRE_EQ(measured, expected);
        REQUIRE_EQ(sz_fault_live_allocations(), live);
        REQUIRE_EQ(fixture.samples, originalSamples);
        REQUIRE_TRUE(std::ranges::equal(originalDictionary, std::span(selection->dictContent, selection->dictSize)));
    }
}

// Purpose: Prevent declared sample extent overflow from reaching either dictionary trainer.
// Inputs: Live size arrays at zero, maximum and overflowing accumulation boundaries.
// Outputs: Requires exact sums or the rejecting SIZE_MAX sentinel without wrapping.
TEST_CASE(zstd_cover_sample_sum_rejects_overflow) {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const std::array<std::size_t, 3> valid{0U, 8U, 17U};
    const std::array<std::size_t, 2> overflow{maximum, 1U};
    REQUIRE_EQ(COVER_sum(valid.data(), 3U), 25U);
    REQUIRE_EQ(COVER_sum(overflow.data(), 2U), maximum);
    REQUIRE_EQ(COVER_sum(nullptr, 1U), maximum);
    REQUIRE_EQ(COVER_sum(nullptr, 0U), 0U);
}

// Purpose: Verify fixture ownership independently of decoder construction and fault injection.
// Inputs: All record slots, one excess allocation, null/foreign/double releases, and a reset with live storage.
// Outputs: Requires bounded failure, preserved owners and bytes, matching cleanup and exact diagnostics.
TEST_CASE(zstd_fault_allocator_preserves_exclusive_records) {
    REQUIRE_TRUE(sz_fault_reset(0U));
    std::array<void*, 32> records{};
    for (std::size_t index = 0; index < records.size(); ++index) {
        records[index] = sz_fault_malloc(16U);
        REQUIRE_TRUE(records[index] != nullptr);
        static_cast<BYTE*>(records[index])[0] = static_cast<BYTE>(index);
    }
    REQUIRE_EQ(sz_fault_live_allocations(), records.size());
    REQUIRE_TRUE(sz_fault_malloc(16U) == nullptr);
    REQUIRE_TRUE(!sz_fault_reset(1U));
    BYTE foreign{};
    sz_fault_free(&foreign);
    sz_fault_free(nullptr);
    REQUIRE_EQ(sz_fault_live_allocations(), records.size());
    REQUIRE_EQ(sz_fault_invalid_frees(), 1U);
    for (std::size_t index = 0; index < records.size(); ++index) {
        REQUIRE_EQ(static_cast<BYTE*>(records[index])[0], static_cast<BYTE>(index));
        sz_fault_free(records[index]);
    }
    sz_fault_free(records.front());
    REQUIRE_EQ(sz_fault_invalid_frees(), 2U);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_TRUE(sz_fault_reset(1U));
    REQUIRE_TRUE(sz_fault_malloc(16U) == nullptr);
    REQUIRE_EQ(sz_fault_allocation_attempts(), 1U);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
}
