#include "test_util.hpp"

#define ZSTD_STATIC_LINKING_ONLY
#define ZDICT_STATIC_LINKING_ONLY
#include "zdict.h"
#include "zstd.h"
#include "zstd_errors.h"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

namespace {

// Purpose: Release test-owned virtual pages on every normal or assertion exit.
// Inputs: address is null or one VirtualAlloc allocation base.
// Outputs: Returns its complete reservation without throwing.
struct VirtualPageDeleter {
    void operator()(unsigned char* address) const noexcept {
        if (address != nullptr) {
            (void)VirtualFree(address, 0, MEM_RELEASE);
        }
    }
};

// Tail placement covers every alignment while making an out-of-extent access fault in this bounded test process.
class GuardedBuffer {
  public:
    // Purpose: Put an exact bounded extent immediately before a verified unreadable page.
    // Inputs: bytes is at most 1 MiB; zero exposes a one-past-end pointer without readable bytes.
    // Outputs: Owns protected pages plus a writable extent; throws on failed allocation/protection/control checks.
    explicit GuardedBuffer(std::size_t bytes) : size_(bytes) {
        REQUIRE_TRUE(bytes <= 1024U * 1024U);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto page = static_cast<std::size_t>(system.dwPageSize);
        REQUIRE_TRUE(page != 0U);
        accessible_ = std::max(page, ((bytes + page - 1U) / page) * page);
        allocation_.reset(static_cast<unsigned char*>(
            VirtualAlloc(nullptr, accessible_ + 2U * page, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS)));
        REQUIRE_TRUE(allocation_ != nullptr);
        begin_ = allocation_.get() + page;
        DWORD previous = 0;
        REQUIRE_TRUE(VirtualProtect(begin_, accessible_, PAGE_READWRITE, &previous) != 0);
        std::fill_n(begin_, accessible_, kCanary);
        data_ = begin_ + accessible_ - bytes;
        unsigned char probe = 0;
        SIZE_T received = 0;
        REQUIRE_TRUE(ReadProcessMemory(GetCurrentProcess(), begin_ + accessible_, &probe, 1U, &received) == 0);
        REQUIRE_EQ(received, 0U);
    }

    // Purpose: Borrow the exactly sized writable test extent without transferring its page owner.
    // Inputs: None; this owner must outlive every compression/hash call.
    // Outputs: Returns the extent, including an empty span backed by an unreadable one-past-end address.
    std::span<unsigned char> bytes() const noexcept {
        return {data_, size_};
    }

    // Purpose: Detect writes before the advertised destination extent.
    // Inputs: The live owned pages after a success or error result.
    // Outputs: Requires every preceding canary byte unchanged; the following guard enforces the upper bound.
    void check_prefix() const {
        REQUIRE_TRUE(std::all_of(begin_, data_, [](unsigned char value) { return value == kCanary; }));
    }

  private:
    static constexpr unsigned char kCanary = 0xA5U;
    std::unique_ptr<unsigned char, VirtualPageDeleter> allocation_;
    unsigned char* begin_ = nullptr;
    unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t accessible_ = 0;
};

struct Samples {
    std::vector<unsigned char> bytes;
    std::array<std::size_t, 64> sizes{};

    // Purpose: Build a deterministic training corpus with varying records and bounded total memory.
    // Inputs: None; no file or host identity enters sample bytes.
    // Outputs: Owns 64 independent 512-byte samples suitable for successful COVER/FASTCOVER controls.
    Samples() {
        for (std::size_t record = 0; record < sizes.size(); ++record) {
            const auto text = "record=" + std::to_string(record) + " compression archive dictionary boundary sample\n";
            sizes[record] = 512U;
            for (std::size_t byte = 0; byte < sizes[record]; ++byte) {
                bytes.push_back(static_cast<unsigned char>(text[byte % text.size()]));
            }
        }
    }
};

using CompressionOwner = std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>;
using DecompressionOwner = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>;

// Purpose: Require a trained dictionary to compress and decode real independent sample bytes.
// Inputs: dictionary is a successful complete dictionary; samples owns its deterministic source records.
// Outputs: Requires successful production-DLL compression, exact read-back and untouched output canaries.
void check_dictionary_readback(std::span<const unsigned char> dictionary, const Samples& samples) {
    CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(encoder != nullptr && decoder != nullptr);
    const auto source = std::span(samples.bytes).last(samples.sizes.back());
    GuardedBuffer frame(ZSTD_compressBound(source.size()));
    const auto encoded = ZSTD_compress_usingDict(encoder.get(), frame.bytes().data(), frame.bytes().size(),
                                                 source.data(), source.size(), dictionary.data(), dictionary.size(), 3);
    REQUIRE_TRUE(!ZSTD_isError(encoded));
    REQUIRE_TRUE(encoded <= frame.bytes().size());
    GuardedBuffer restored(source.size());
    const auto decoded = ZSTD_decompress_usingDict(decoder.get(), restored.bytes().data(), restored.bytes().size(),
                                                   frame.bytes().data(), encoded, dictionary.data(), dictionary.size());
    REQUIRE_EQ(decoded, source.size());
    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source));
    frame.check_prefix();
    restored.check_prefix();
}

// Purpose: Exercise compression error/success transitions at exact guarded destination capacities.
// Inputs: source is at most 64 KiB, level is a shipped effort, and context is exclusively borrowed.
// Outputs: Requires bounded writes/read-back and only destination-size errors below compressBound; exact final
// frame size is not a sufficient workspace-capacity promise in Zstandard's API.
void check_frame_capacities(std::span<const unsigned char> source, int level, ZSTD_CCtx* context) {
    std::vector<unsigned char> reference(ZSTD_compressBound(source.size()));
    const auto required =
        ZSTD_compressCCtx(context, reference.data(), reference.size(), source.data(), source.size(), level);
    REQUIRE_TRUE(!ZSTD_isError(required));
    REQUIRE_TRUE(required != 0U && required <= reference.size());
    const std::array capacities{std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                                std::size_t{4}, std::size_t{5}, std::size_t{6}, std::size_t{7},
                                required - 1U,  required,       required + 1U,  reference.size()};
    for (const auto capacity : capacities) {
        GuardedBuffer output(capacity);
        const auto written =
            ZSTD_compressCCtx(context, output.bytes().data(), capacity, source.data(), source.size(), level);
        output.check_prefix();
        if (ZSTD_isError(written)) {
            REQUIRE_EQ(ZSTD_getErrorCode(written), ZSTD_error_dstSize_tooSmall);
            REQUIRE_TRUE(capacity < reference.size());
            continue;
        }
        REQUIRE_TRUE(written != 0U && written <= capacity);
        if (capacity == reference.size()) {
            REQUIRE_EQ(written, required);
            REQUIRE_TRUE(std::equal(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(required),
                                    output.bytes().begin()));
        }
        GuardedBuffer restored(source.size());
        REQUIRE_EQ(ZSTD_decompress(restored.bytes().data(), source.size(), output.bytes().data(), written),
                   source.size());
        REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source));
        restored.check_prefix();
    }
}

}  // namespace

// Purpose: Cover suffix hash remainders/alignment without reading beyond the supplied dictionary content.
// Inputs: Guarded suffixes of lengths 0-65 and larger boundary lengths, with paired ordinary inputs.
// Outputs: Requires successful byte-identical final dictionaries and actual dictionary compression/read-back.
TEST_CASE(zstd_dictionary_guarded_suffix_hash_boundaries) {
    REQUIRE_EQ(ZSTD_versionNumber(), 10507U);
    const Samples samples;
    std::vector<std::size_t> lengths;
    for (std::size_t length = 0; length <= 65U; ++length) {
        lengths.push_back(length);
    }
    for (const auto length : {127U, 255U, 256U, 511U, 1023U, 2047U, 4095U, 4096U}) {
        lengths.push_back(length);
    }
    for (const auto length : lengths) {
        GuardedBuffer content(length);
        for (std::size_t byte = 0; byte < length; ++byte) {
            content.bytes()[byte] = static_cast<unsigned char>((byte * 73U + 19U) % 256U);
        }
        const std::vector<unsigned char> ordinary(content.bytes().begin(), content.bytes().end());
        GuardedBuffer dictionary(8192U);
        std::vector<unsigned char> reference(8192U);
        const ZDICT_params_t parameters{3, 0, 0};
        const auto size = ZDICT_finalizeDictionary(
            dictionary.bytes().data(), dictionary.bytes().size(), content.bytes().data(), length, samples.bytes.data(),
            samples.sizes.data(), static_cast<unsigned>(samples.sizes.size()), parameters);
        const auto expected =
            ZDICT_finalizeDictionary(reference.data(), reference.size(), ordinary.data(), length, samples.bytes.data(),
                                     samples.sizes.data(), static_cast<unsigned>(samples.sizes.size()), parameters);
        REQUIRE_TRUE(!ZDICT_isError(size));
        REQUIRE_EQ(size, expected);
        REQUIRE_TRUE(size <= dictionary.bytes().size());
        REQUIRE_TRUE(std::equal(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(size),
                                dictionary.bytes().begin()));
        content.check_prefix();
        dictionary.check_prefix();
        check_dictionary_readback(dictionary.bytes().first(size), samples);
    }
}

// Purpose: Bound both dictionary builders and optimizer/shrinking variants by their advertised output extent.
// Inputs: Small invalid and successful guarded capacities, two hash widths and both shrink policies.
// Outputs: Requires error results below minimum capacity and successful dictionary read-back for larger controls.
TEST_CASE(zstd_cover_fastcover_guarded_training_capacities) {
    const Samples samples;
    for (const auto capacity : {0U, 1U, 255U, 256U, 512U, 2048U}) {
        for (const auto width : {6U, 8U}) {
            for (const auto shrink : {0U, 1U}) {
                for (const auto method : {0U, 1U, 2U, 3U}) {
                    GuardedBuffer dictionary(capacity);
                    ZDICT_cover_params_t cover{};
                    cover.k = 64U;
                    cover.d = width;
                    cover.steps = 1U;
                    cover.nbThreads = 2U;
                    cover.splitPoint = 0.75;
                    cover.shrinkDict = shrink;
                    cover.zParams.compressionLevel = 3;
                    ZDICT_fastCover_params_t fast{};
                    fast.k = cover.k;
                    fast.d = width;
                    fast.f = 10U;
                    fast.steps = cover.steps;
                    fast.nbThreads = cover.nbThreads;
                    fast.splitPoint = cover.splitPoint;
                    fast.shrinkDict = shrink;
                    fast.zParams = cover.zParams;
                    const auto count = static_cast<unsigned>(samples.sizes.size());
                    auto* output = dictionary.bytes().data();
                    const auto size =
                        method == 0U   ? ZDICT_trainFromBuffer_cover(output, capacity, samples.bytes.data(),
                                                                     samples.sizes.data(), count, cover)
                        : method == 1U ? ZDICT_optimizeTrainFromBuffer_cover(output, capacity, samples.bytes.data(),
                                                                             samples.sizes.data(), count, &cover)
                        : method == 2U ? ZDICT_trainFromBuffer_fastCover(output, capacity, samples.bytes.data(),
                                                                         samples.sizes.data(), count, fast)
                                       : ZDICT_optimizeTrainFromBuffer_fastCover(output, capacity, samples.bytes.data(),
                                                                                 samples.sizes.data(), count, &fast);
                    dictionary.check_prefix();
                    if (capacity < ZDICT_DICTSIZE_MIN) {
                        REQUIRE_TRUE(ZDICT_isError(size));
                    } else if (!ZDICT_isError(size)) {
                        REQUIRE_TRUE(size <= capacity);
                        check_dictionary_readback(dictionary.bytes().first(size), samples);
                    } else {
                        REQUIRE_TRUE(capacity < 2048U);
                    }
                }
            }
        }
    }
}

// Purpose: Exercise empty, RLE, raw and compressed frame writes at exact capacities and source boundaries.
// Inputs: Three deterministic content families, tiny/block-boundary extents and efforts 1/5/9.
// Outputs: Requires bounded production writes, successful compressBound controls and byte-exact decompression.
TEST_CASE(zstd_frame_guarded_capacity_and_source_boundaries) {
    CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
    REQUIRE_TRUE(encoder != nullptr);
    for (const auto length : {0U, 1U, 2U, 3U, 127U, 128U, 129U, 255U, 256U, 4096U, 65536U}) {
        for (const auto family : {0U, 1U, 2U}) {
            GuardedBuffer input(length);
            std::uint32_t random = 0x913579BDU;
            for (auto& byte : input.bytes()) {
                random ^= random << 13U;
                random ^= random >> 17U;
                random ^= random << 5U;
                byte = family == 0U ? 0U : static_cast<unsigned char>(family == 1U ? random : random % 7U);
            }
            for (const auto level : {1, 5, 9}) {
                check_frame_capacities(input.bytes(), level, encoder.get());
            }
            input.check_prefix();
        }
    }
}
