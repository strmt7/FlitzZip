#include "test_util.hpp"
#include "zstd_legacy_fixture.hpp"
#include "raw_block_probe.h"

#define ZSTD_STATIC_LINKING_ONLY
#define ZDICT_STATIC_LINKING_ONLY
#include "zdict.h"
#include "zstd.h"
#include "zstd_errors.h"

#include <algorithm>
#include <array>
#include <memory>
#include <limits>
#include <span>
#include <thread>

namespace {

struct ScopedThreadErrorMode {
    DWORD previous = 0;

    // Purpose: Keep an intentionally isolated guard-page failure from opening a host error dialog.
    // Inputs: None; affects only the current test thread. Outputs: Saves its previous Windows error mode.
    ScopedThreadErrorMode() {
        REQUIRE_TRUE(SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, &previous) != 0);
    }

    // Purpose: Restore test-thread error handling on every ordinary/assertion exit.
    // Inputs: The saved mode. Outputs: Restores the original thread setting without throwing.
    ~ScopedThreadErrorMode() {
        (void)SetThreadErrorMode(previous, nullptr);
    }
};

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

// Purpose: Require the production shared-library allocator data ABI and default factory semantics.
// Inputs: The actual imported immutable allocator value; contexts own their allocations.
// Outputs: Both advanced factories construct valid contexts with unchanged null callbacks.
TEST_CASE(zstd_default_allocator_shared_library_linkage) {
    REQUIRE_TRUE(ZSTD_defaultCMem.customAlloc == nullptr);
    REQUIRE_TRUE(ZSTD_defaultCMem.customFree == nullptr);
    REQUIRE_TRUE(ZSTD_defaultCMem.opaque == nullptr);
    CompressionOwner compressor(ZSTD_createCCtx_advanced(ZSTD_defaultCMem), &ZSTD_freeCCtx);
    DecompressionOwner decoder(ZSTD_createDCtx_advanced(ZSTD_defaultCMem), &ZSTD_freeDCtx);
    REQUIRE_TRUE(compressor != nullptr);
    REQUIRE_TRUE(decoder != nullptr);
}

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

// Purpose: Encode one unsigned wire field without host-endian assumptions.
// Inputs: output is fixture-owned; value fits width bytes, with width at most four.
// Outputs: Appends exactly width little-endian bytes.
void append_wire_integer(std::vector<unsigned char>& output, std::uint32_t value, unsigned width) {
    for (unsigned byte = 0; byte < width; ++byte) {
        output.push_back(static_cast<unsigned char>(value >> (byte * 8U)));
    }
}

// Purpose: Construct a complete frame with one raw or RLE literal section and zero sequences.
// Inputs: expected contains 1-131072 decoded bytes; width selects a legal literal header; rle requires equal bytes.
// Outputs: Returns a fixture using a 128 KiB window, four-byte content size and one final compressed block.
std::vector<unsigned char> make_literal_frame(std::span<const unsigned char> expected, unsigned width, bool rle) {
    REQUIRE_TRUE(!expected.empty() && expected.size() <= 131072U);
    REQUIRE_TRUE(width >= 1U && width <= 3U);
    const std::uint32_t code = width == 1U ? 0U : (width == 2U ? 1U : 3U);
    const auto size = static_cast<std::uint32_t>(expected.size());
    const auto literal_header = (size << (width == 1U ? 3U : 4U)) | (code << 2U) | (rle ? 1U : 0U);
    std::vector<unsigned char> block;
    append_wire_integer(block, literal_header, width);
    if (rle) {
        REQUIRE_TRUE(std::ranges::all_of(expected, [&](auto byte) { return byte == expected.front(); }));
        block.push_back(expected.front());
    } else {
        block.insert(block.end(), expected.begin(), expected.end());
    }
    block.push_back(0U);  // No sequences: the complete output consists of literals.
    REQUIRE_TRUE(block.size() <= 131072U);
    std::vector<unsigned char> frame;
    append_wire_integer(frame, ZSTD_MAGICNUMBER, 4U);
    frame.push_back(0x80U);  // Four-byte content size with an explicit window descriptor.
    frame.push_back(0x38U);  // 128 KiB permits a compressed block larger than a tiny decoded payload.
    append_wire_integer(frame, size, 4U);
    append_wire_integer(frame, (static_cast<std::uint32_t>(block.size()) << 3U) | 5U, 3U);
    frame.insert(frame.end(), block.begin(), block.end());
    return frame;
}

// Purpose: Construct legacy raw/RLE literal frames with explicit version-specific wire bytes.
// Inputs: version is 5-7; expected has 1-131072 decoded bytes; width selects a legal header; RLE bytes agree.
// Outputs: Returns one compressed block with zero sequences and the legacy end marker, without a dictionary.
std::vector<unsigned char> make_legacy_literal_frame(unsigned version, std::span<const unsigned char> expected,
                                                     unsigned width, bool rle) {
    REQUIRE_TRUE(version >= 5U && version <= 7U);
    REQUIRE_TRUE(!expected.empty() && expected.size() <= 131072U);
    REQUIRE_TRUE(width >= 1U && width <= 3U);
    const auto size = static_cast<std::uint32_t>(expected.size());
    REQUIRE_TRUE(width != 1U || size <= 31U);
    REQUIRE_TRUE(width != 2U || size <= 4095U);
    std::vector<unsigned char> block;
    const auto kind = rle ? 0xC0U : 0x80U;
    const auto code = width == 1U ? 0U : width;
    block.push_back(static_cast<unsigned char>(kind | (code << 4U) | (size >> ((width - 1U) * 8U))));
    for (auto remaining = width - 1U; remaining != 0U; --remaining) {
        block.push_back(static_cast<unsigned char>(size >> ((remaining - 1U) * 8U)));
    }
    if (rle) {
        REQUIRE_TRUE(std::ranges::all_of(expected, [&](auto byte) { return byte == expected.front(); }));
        block.push_back(expected.front());
    } else {
        block.insert(block.end(), expected.begin(), expected.end());
    }
    block.push_back(0U);
    REQUIRE_TRUE(block.size() < 131072U);
    std::vector<unsigned char> frame{static_cast<unsigned char>(0x20U + version), 0xB5U, 0x2FU, 0xFDU};
    if (version == 7U) {
        frame.insert(frame.end(), {0x00U, 0x38U});
    } else {
        frame.push_back(0x07U);
    }
    const auto encoded_size = static_cast<std::uint32_t>(block.size());
    frame.push_back(static_cast<unsigned char>(encoded_size >> 16U));
    frame.push_back(static_cast<unsigned char>(encoded_size >> 8U));
    frame.push_back(static_cast<unsigned char>(encoded_size));
    frame.insert(frame.end(), block.begin(), block.end());
    frame.insert(frame.end(), {0xC0U, 0x00U, 0x00U});
    return frame;
}

// Purpose: Exercise literal decoding through the public production-DLL frame API with unreadable boundaries.
// Inputs: frame is complete and expected owns the exact decoded bytes.
// Outputs: Requires exact successful output, bounded capacity errors, nonempty truncation rejection and empty-input
// zero.
void require_literal_frame(std::span<const unsigned char> frame, std::span<const unsigned char> expected) {
    GuardedBuffer input(frame.size());
    std::ranges::copy(frame, input.bytes().begin());
    GuardedBuffer output(expected.size());
    REQUIRE_EQ(ZSTD_decompress(output.bytes().data(), output.bytes().size(), input.bytes().data(), frame.size()),
               expected.size());
    REQUIRE_TRUE(std::ranges::equal(output.bytes(), expected));
    output.check_prefix();
    GuardedBuffer small(expected.size() - 1U);
    REQUIRE_TRUE(ZSTD_isError(
        ZSTD_decompress(small.bytes().data(), small.bytes().size(), input.bytes().data(), input.bytes().size())));
    small.check_prefix();
    GuardedBuffer empty(0U);
    REQUIRE_EQ(ZSTD_decompress(output.bytes().data(), output.bytes().size(), empty.bytes().data(), 0U), 0U);
    for (const auto cut : {std::size_t{1}, std::size_t{4}, std::size_t{8}, frame.size() - 1U}) {
        GuardedBuffer truncated(cut);
        std::ranges::copy(frame.first(cut), truncated.bytes().begin());
        REQUIRE_TRUE(
            ZSTD_isError(ZSTD_decompress(output.bytes().data(), output.bytes().size(), truncated.bytes().data(), cut)));
        truncated.check_prefix();
        output.check_prefix();
    }
    input.check_prefix();
}

// Purpose: Record which literal encoding branches real product frames exercise.
// Inputs: frame is a complete checksum-free standard frame; counts stores four literal kinds.
// Outputs: Validates block framing and increments observed literal kinds without parsing entropy payloads.
void count_literal_encodings(std::span<const unsigned char> frame, std::array<std::size_t, 4>& counts) {
    ZSTD_FrameHeader header{};
    REQUIRE_EQ(ZSTD_getFrameHeader(&header, frame.data(), frame.size()), 0U);
    REQUIRE_EQ(header.checksumFlag, 0U);
    auto position = static_cast<std::size_t>(header.headerSize);
    bool last = false;
    while (!last) {
        REQUIRE_TRUE(position <= frame.size() && frame.size() - position >= 3U);
        const auto block = static_cast<std::uint32_t>(frame[position]) |
                           (static_cast<std::uint32_t>(frame[position + 1U]) << 8U) |
                           (static_cast<std::uint32_t>(frame[position + 2U]) << 16U);
        position += 3U;
        last = (block & 1U) != 0U;
        const auto type = (block >> 1U) & 3U;
        REQUIRE_TRUE(type != 3U);
        const auto size = type == 1U ? std::size_t{1} : static_cast<std::size_t>(block >> 3U);
        REQUIRE_TRUE(size <= frame.size() - position);
        if (type == 2U) {
            REQUIRE_TRUE(size >= 2U);
            ++counts[frame[position] & 3U];
        }
        position += size;
    }
    REQUIRE_EQ(position, frame.size());
}

// Purpose: Preserve streaming literal placement with independent small input/output chunks.
// Inputs: frame is complete; expected contains its decoded bytes; chunk sizes are bounded fixture choices.
// Outputs: Requires progress, exact read-back, complete frame consumption and untouched output canaries.
void require_literal_stream(std::span<const unsigned char> frame, std::span<const unsigned char> expected,
                            std::size_t input_chunk, std::size_t output_chunk) {
    REQUIRE_TRUE(input_chunk > 0U && output_chunk > 0U && output_chunk <= 4096U);
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_initDStream(decoder.get())));
    constexpr std::size_t guard = 16U;
    constexpr unsigned char canary = 0xA5U;
    std::array<unsigned char, 4096U + 2U * guard> chunk{};
    std::vector<unsigned char> restored;
    restored.reserve(expected.size());
    std::size_t consumed = 0U;
    std::size_t calls = 0U;
    for (;;) {
        REQUIRE_TRUE(++calls <= frame.size() + expected.size() + 1U);
        const auto available = std::min(input_chunk, frame.size() - consumed);
        ZSTD_inBuffer input{frame.data() + consumed, available, 0U};
        std::ranges::fill(chunk, canary);
        ZSTD_outBuffer output{chunk.data() + guard, output_chunk, 0U};
        const auto next = ZSTD_decompressStream(decoder.get(), &output, &input);
        REQUIRE_TRUE(!ZSTD_isError(next));
        REQUIRE_TRUE(input.pos <= available && output.pos <= output_chunk);
        REQUIRE_TRUE(std::all_of(chunk.begin(), chunk.begin() + guard, [](auto byte) { return byte == canary; }));
        REQUIRE_TRUE(std::all_of(chunk.begin() + static_cast<std::ptrdiff_t>(guard + output_chunk), chunk.end(),
                                 [](auto byte) { return byte == canary; }));
        consumed += input.pos;
        REQUIRE_TRUE(output.pos <= expected.size() - restored.size());
        restored.insert(restored.end(), chunk.begin() + guard,
                        chunk.begin() + static_cast<std::ptrdiff_t>(guard + output.pos));
        if (next == 0U) {
            REQUIRE_EQ(consumed, frame.size());
            break;
        }
        REQUIRE_TRUE(input.pos != 0U || output.pos != 0U);
    }
    REQUIRE_TRUE(std::ranges::equal(restored, expected));
}

// Purpose: Exercise the public bufferless state machine with exactly bounded input chunks and retained output.
// Inputs: frame owns a complete modern/skippable frame; output owns guarded writable storage for its full plaintext.
// Outputs: Returns decoded bytes or a native error, checking chunk/canary boundaries before every exit.
std::size_t decode_bufferless(std::span<const unsigned char> frame, GuardedBuffer& output) {
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    REQUIRE_EQ(ZSTD_decompressBegin(decoder.get()), 0U);
    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (std::size_t calls = 0; calls <= frame.size() + 16U; ++calls) {
        const auto needed = ZSTD_nextSrcSizeToDecompress(decoder.get());
        if (needed == 0U) {
            REQUIRE_EQ(consumed, frame.size());
            return produced;
        }
        REQUIRE_TRUE(consumed <= frame.size() && needed <= frame.size() - consumed);
        REQUIRE_TRUE(produced <= output.bytes().size());
        GuardedBuffer chunk(needed);
        std::ranges::copy(frame.subspan(consumed, needed), chunk.bytes().begin());
        const auto result = ZSTD_decompressContinue(decoder.get(), output.bytes().data() + produced,
                                                    output.bytes().size() - produced, chunk.bytes().data(), needed);
        chunk.check_prefix();
        output.check_prefix();
        if (ZSTD_isError(result)) {
            return result;
        }
        REQUIRE_TRUE(result <= output.bytes().size() - produced);
        consumed += needed;
        produced += result;
    }
    throw std::runtime_error("Bufferless decoder exceeded its bounded progress limit.");
}

// Purpose: Require exact modern literal decoding through the public bufferless state API.
// Inputs: frame owns complete wire bytes and expected is its independent plaintext oracle.
// Outputs: Requires exact byte count/read-back and bounded insufficient-capacity failure.
void require_literal_bufferless(std::span<const unsigned char> frame, std::span<const unsigned char> expected) {
    REQUIRE_TRUE(!expected.empty());
    GuardedBuffer output(expected.size());
    REQUIRE_EQ(decode_bufferless(frame, output), expected.size());
    REQUIRE_TRUE(std::ranges::equal(output.bytes(), expected));
    GuardedBuffer small(expected.size() - 1U);
    REQUIRE_TRUE(ZSTD_isError(decode_bufferless(frame, small)));
}

// Purpose: Exercise streaming compression's buffered/stable loading, flushing and frame completion.
// Inputs: source remains live; stable modes retain each complete buffer; flush_mid requests an explicit partial flush.
// Outputs: Returns bounded complete frame bytes; requires progress, exact consumption and unchanged guard regions.
std::vector<unsigned char> compress_stream_frame(std::span<const unsigned char> source, bool stable_input,
                                                 bool stable_output, bool flush_mid) {
    CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
    REQUIRE_TRUE(encoder != nullptr);
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_compressionLevel, 5)));
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_stableInBuffer, stable_input)));
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_stableOutBuffer, stable_output)));
    REQUIRE_TRUE(!stable_input || !flush_mid);
    GuardedBuffer output(stable_output ? ZSTD_compressBound(source.size()) : 31U);
    std::vector<unsigned char> frame;
    std::size_t consumed = 0U;
    std::size_t output_pos = 0U;
    std::size_t calls = 0U;
    bool flushed = !flush_mid;
    for (;;) {
        REQUIRE_TRUE(++calls <= source.size() + ZSTD_compressBound(source.size()) + 32U);
        const auto available = stable_input ? source.size() : std::min(std::size_t{4096}, source.size() - consumed);
        const auto* input_base = stable_input || source.empty() ? source.data() : source.data() + consumed;
        ZSTD_inBuffer input{input_base, available, stable_input ? consumed : 0U};
        const auto directive = stable_input && calls == 1U && !source.empty() ? ZSTD_e_continue
                               : !flushed && consumed >= source.size() / 2U   ? ZSTD_e_flush
                               : consumed + (stable_input ? available - consumed : available) == source.size()
                                   ? ZSTD_e_end
                                   : ZSTD_e_continue;
        const auto before = input.pos;
        ZSTD_outBuffer destination{output.bytes().data(), output.bytes().size(), stable_output ? output_pos : 0U};
        const auto before_output = destination.pos;
        const auto remaining = ZSTD_compressStream2(encoder.get(), &destination, &input, directive);
        output.check_prefix();
        REQUIRE_TRUE(!ZSTD_isError(remaining));
        REQUIRE_TRUE(input.pos <= input.size && destination.pos <= destination.size);
        consumed += input.pos - before;
        frame.insert(frame.end(), output.bytes().begin() + static_cast<std::ptrdiff_t>(before_output),
                     output.bytes().begin() + static_cast<std::ptrdiff_t>(destination.pos));
        output_pos = destination.pos;
        if (directive == ZSTD_e_flush && remaining == 0U) {
            flushed = true;
        }
        if (directive == ZSTD_e_end && remaining == 0U) {
            REQUIRE_EQ(consumed, source.size());
            REQUIRE_TRUE(flushed);
            return frame;
        }
        REQUIRE_TRUE(input.pos != before || destination.pos != before_output || directive == ZSTD_e_flush);
    }
}

// Purpose: Exercise actual multiworker rsync loading, cut points and pending-output backpressure.
// Inputs: source stays live; workers selects one/two product workers; input_chunk straddles the minimum sync extent.
// Outputs: Returns a complete bounded frame with exact input consumption and guarded output writes.
std::vector<unsigned char> compress_rsync_frame(std::span<const unsigned char> source, int workers,
                                                std::size_t input_chunk) {
    CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
    REQUIRE_TRUE(encoder != nullptr && input_chunk != 0U && !source.empty());
    for (const auto [parameter, value] :
         {std::pair{ZSTD_c_compressionLevel, 5}, std::pair{ZSTD_c_nbWorkers, workers},
          std::pair{ZSTD_c_jobSize, 512 * 1024}, std::pair{ZSTD_c_overlapLog, 1}, std::pair{ZSTD_c_rsyncable, 1}}) {
        REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), parameter, value)));
    }
    GuardedBuffer output(16U * 1024U);
    std::vector<unsigned char> frame;
    std::size_t consumed = 0U;
    std::size_t idle_calls = 0U;
    for (std::size_t calls = 0; calls < 200000U; ++calls) {
        const auto available = std::min(input_chunk, source.size() - consumed);
        ZSTD_inBuffer input{source.data() + consumed, available, 0U};
        ZSTD_outBuffer destination{output.bytes().data(), output.bytes().size(), 0U};
        const auto directive = consumed + available == source.size() ? ZSTD_e_end : ZSTD_e_continue;
        const auto remaining = ZSTD_compressStream2(encoder.get(), &destination, &input, directive);
        output.check_prefix();
        REQUIRE_TRUE(!ZSTD_isError(remaining));
        REQUIRE_TRUE(input.pos <= input.size && destination.pos <= destination.size);
        consumed += input.pos;
        frame.insert(frame.end(), output.bytes().begin(),
                     output.bytes().begin() + static_cast<std::ptrdiff_t>(destination.pos));
        REQUIRE_TRUE(frame.size() <= ZSTD_compressBound(source.size()));
        if (directive == ZSTD_e_end && remaining == 0U) {
            REQUIRE_EQ(consumed, source.size());
            return frame;
        }
        if (input.pos == 0U && destination.pos == 0U) {
            REQUIRE_TRUE(++idle_calls < 100000U);
            std::this_thread::yield();
        } else {
            idle_calls = 0U;
        }
    }
    throw std::runtime_error("Multiworker rsync compressor exceeded its bounded progress limit.");
}

// Purpose: Establish stable-output and magicless stream-header state contracts through the public DLL.
// Inputs: frame owns complete modern bytes; expected is independent plaintext; magicless omits the four magic bytes.
// Outputs: Requires bounded progress, exact consumption/read-back and intact output guard regions.
void require_stable_stream(std::span<const unsigned char> frame, std::span<const unsigned char> expected,
                           bool magicless, std::size_t input_chunk) {
    REQUIRE_TRUE(frame.size() >= 4U && input_chunk > 0U);
    if (magicless) {
        frame = frame.subspan(4U);
    }
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_stableOutBuffer, 1)));
    REQUIRE_TRUE(!ZSTD_isError(
        ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_format, magicless ? ZSTD_f_zstd1_magicless : ZSTD_f_zstd1)));
    GuardedBuffer restored(expected.size());
    std::size_t consumed = 0U;
    ZSTD_outBuffer output{restored.bytes().data(), restored.bytes().size(), 0U};
    for (std::size_t calls = 0; calls <= frame.size() + expected.size() + 16U; ++calls) {
        const auto available = std::min(input_chunk, frame.size() - consumed);
        ZSTD_inBuffer input{frame.data() + consumed, available, 0U};
        const auto before = output.pos;
        const auto result = ZSTD_decompressStream(decoder.get(), &output, &input);
        restored.check_prefix();
        REQUIRE_TRUE(!ZSTD_isError(result));
        REQUIRE_TRUE(input.pos <= input.size && output.pos <= output.size);
        consumed += input.pos;
        if (result == 0U) {
            REQUIRE_EQ(consumed, frame.size());
            REQUIRE_EQ(output.pos, expected.size());
            REQUIRE_TRUE(std::ranges::equal(restored.bytes(), expected));
            return;
        }
        REQUIRE_TRUE(input.pos != 0U || output.pos != before);
    }
    throw std::runtime_error("Stable stream decoder exceeded its bounded progress limit.");
}

}  // namespace

// Purpose: Preserve caller-owned buffer identities while a legacy frame waits for output storage.
// Inputs: version selects an upstream golden frame; its complete input remains live through all stream calls.
// Outputs: Requires a positive hint until every byte is flushed, unchanged public pointers and exact read-back.
void require_legacy_public_output_backpressure(unsigned version) {
    const auto extent = superzip_test::kLegacyFrames[version - 4U];
    const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    ZSTD_inBuffer input{frame.data(), frame.size(), 0U};
    ZSTD_outBuffer output{nullptr, 0U, 0U};
    auto hint = ZSTD_decompressStream(decoder.get(), &output, &input);
    REQUIRE_TRUE(!ZSTD_isError(hint));
    REQUIRE_TRUE(output.dst == nullptr);
    REQUIRE_EQ(output.pos, 0U);
    REQUIRE_TRUE(hint > 0U);
    REQUIRE_TRUE(input.src == frame.data());
    REQUIRE_TRUE(input.pos <= input.size);
    GuardedBuffer restored(superzip_test::kLegacyExpected.size());
    output = {restored.bytes().data(), restored.bytes().size(), 0U};
    for (unsigned call = 0; hint != 0U && call < 16U; ++call) {
        ZSTD_inBuffer remaining{input.pos != input.size ? frame.data() + input.pos : nullptr, input.size - input.pos,
                                0U};
        const auto* const original_input = remaining.src;
        hint = ZSTD_decompressStream(decoder.get(), &output, &remaining);
        REQUIRE_TRUE(!ZSTD_isError(hint));
        REQUIRE_TRUE(remaining.src == original_input);
        REQUIRE_TRUE(remaining.pos <= remaining.size);
        input.pos += remaining.pos;
        restored.check_prefix();
    }
    REQUIRE_EQ(hint, 0U);
    REQUIRE_EQ(input.pos, input.size);
    REQUIRE_EQ(output.pos, restored.bytes().size());
    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), superzip_test::kLegacyExpected));
}

// Purpose: Check v0.5 public output backpressure; no inputs, requires preserved buffer identity and complete output.
TEST_CASE(zstd_legacy_v05_public_output_backpressure) {
    require_legacy_public_output_backpressure(5U);
}

// Purpose: Check v0.6 public output backpressure; no inputs, requires preserved buffer identity and complete output.
TEST_CASE(zstd_legacy_v06_public_output_backpressure) {
    require_legacy_public_output_backpressure(6U);
}

// Purpose: Check v0.7 public output backpressure; no inputs, requires preserved buffer identity and complete output.
TEST_CASE(zstd_legacy_v07_public_output_backpressure) {
    require_legacy_public_output_backpressure(7U);
}

// Purpose: Establish synchronization behavior before changing its loop accounting.
// Inputs: Exact guarded payloads, both last-block flags and sufficient/insufficient output extents.
// Outputs: Requires exact raw block header/payload bytes, native capacity errors and untouched guard regions.
TEST_CASE(zstd_raw_block_writer_wire_contracts) {
    for (const auto length : {0U, 1U, 2U, 31U, 128U, 4096U, 131072U}) {
        GuardedBuffer source(length);
        for (std::size_t index = 0; index < source.bytes().size(); ++index) {
            source.bytes()[index] = static_cast<unsigned char>((index * 73U + 19U) & 255U);
        }
        for (const auto last : {0U, 1U}) {
            GuardedBuffer output(length + 3U);
            REQUIRE_EQ(sz_zstd_no_compress_block(output.bytes().data(), output.bytes().size(), source.bytes().data(),
                                                 source.bytes().size(), last),
                       length + 3U);
            const auto header = (length << 3U) | last;
            for (const auto index : {0U, 1U, 2U}) {
                REQUIRE_EQ(output.bytes()[index], static_cast<unsigned char>(header >> (8U * index)));
            }
            REQUIRE_TRUE(std::ranges::equal(output.bytes().subspan(3U), source.bytes()));
            GuardedBuffer small(length + 2U);
            REQUIRE_EQ(ZSTD_getErrorCode(sz_zstd_no_compress_block(small.bytes().data(), small.bytes().size(),
                                                                   source.bytes().data(), source.bytes().size(), last)),
                       ZSTD_error_dstSize_tooSmall);
            output.check_prefix();
            small.check_prefix();
            source.check_prefix();
        }
    }
    GuardedBuffer output(3U);
    REQUIRE_EQ(sz_zstd_no_compress_block(output.bytes().data(), output.bytes().size(), nullptr, 0U, 1U), 3U);
    REQUIRE_EQ(output.bytes()[0], 1U);
    REQUIRE_EQ(output.bytes()[1], 0U);
    REQUIRE_EQ(output.bytes()[2], 0U);
    output.check_prefix();
}

// Purpose: Require declared lengths near SIZE_MAX to fail before any raw-block header or payload access.
// Inputs: A deliberately impossible source length and tiny guarded destinations; run the old-source control alone.
// Outputs: Requires dstSize_tooSmall and intact output guards without touching the one-byte source fixture.
TEST_CASE(zstd_raw_block_writer_overflow_rejection) {
    const ScopedThreadErrorMode error_mode;
    const std::array<unsigned char, 1> source{0x5CU};
    for (const auto capacity : {0U, 1U, 2U, 3U, 4U}) {
        for (const auto length :
             {std::numeric_limits<std::size_t>::max() - 2U, std::numeric_limits<std::size_t>::max() - 1U,
              std::numeric_limits<std::size_t>::max()}) {
            GuardedBuffer output(capacity);
            REQUIRE_EQ(ZSTD_getErrorCode(sz_zstd_no_compress_block(output.bytes().data(), output.bytes().size(),
                                                                   source.data(), length, 0U)),
                       ZSTD_error_dstSize_tooSmall);
            output.check_prefix();
        }
    }
}

// Purpose: Establish synchronization behavior before changing its loop accounting.
// Inputs: Twenty-four corpus/worker/chunk combinations span minimum sync and job boundaries.
// Outputs: Requires byte-exact one-shot and streaming read-back with retained guarded source/output storage.
TEST_CASE(zstd_multiworker_rsync_synchronization_contracts) {
    for (const auto length : {700001U, 1048319U}) {
        for (const auto family : {0U, 1U, 2U}) {
            GuardedBuffer source(length);
            std::uint32_t random = 0xA371942DU;
            for (auto& byte : source.bytes()) {
                random ^= random << 13U;
                random ^= random >> 17U;
                random ^= random << 5U;
                byte = family == 0U ? 0U : static_cast<unsigned char>(family == 1U ? random & 31U : random);
            }
            for (const auto workers : {1, 2}) {
                for (const auto chunk : {131071U, 131073U}) {
                    const auto frame = compress_rsync_frame(source.bytes(), workers, chunk);
                    GuardedBuffer restored(length);
                    REQUIRE_EQ(
                        ZSTD_decompress(restored.bytes().data(), restored.bytes().size(), frame.data(), frame.size()),
                        length);
                    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source.bytes()));
                    require_literal_stream(frame, source.bytes(), 4096U, 4096U);
                    restored.check_prefix();
                    source.check_prefix();
                }
            }
        }
    }
}

// Purpose: Preserve nullable empty-buffer handling in initialized multiworker compression.
// Inputs: Empty source/output buffers after optional one-byte loading, with rsync enabled and disabled.
// Outputs: Requires bounded completion and exact frame read-back for both actual multiworker modes.
TEST_CASE(zstd_multiworker_nullable_empty_buffers) {
    for (const auto workers : {1, 2}) {
        for (const auto rsync : {0, 1}) {
            for (const auto preload : {false, true}) {
                CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
                REQUIRE_TRUE(encoder != nullptr);
                REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_nbWorkers, workers)));
                REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_rsyncable, rsync)));
                const std::array<unsigned char, 1> source{0x5CU};
                ZSTD_outBuffer no_output{nullptr, 0U, 0U};
                if (preload) {
                    ZSTD_inBuffer input{source.data(), source.size(), 0U};
                    REQUIRE_TRUE(
                        !ZSTD_isError(ZSTD_compressStream2(encoder.get(), &no_output, &input, ZSTD_e_continue)));
                    REQUIRE_EQ(input.pos, input.size);
                }
                ZSTD_inBuffer no_input{nullptr, 0U, 0U};
                REQUIRE_TRUE(
                    !ZSTD_isError(ZSTD_compressStream2(encoder.get(), &no_output, &no_input, ZSTD_e_continue)));
                REQUIRE_TRUE(!ZSTD_isError(ZSTD_compressStream2(encoder.get(), &no_output, &no_input, ZSTD_e_end)));
                REQUIRE_EQ(no_input.pos, 0U);
                REQUIRE_EQ(no_output.pos, 0U);
                GuardedBuffer encoded(128U);
                ZSTD_outBuffer output{encoded.bytes().data(), encoded.bytes().size(), 0U};
                REQUIRE_EQ(ZSTD_compressStream2(encoder.get(), &output, &no_input, ZSTD_e_end), 0U);
                const auto expected_size = preload ? source.size() : 0U;
                GuardedBuffer restored(expected_size);
                REQUIRE_EQ(ZSTD_decompress(restored.bytes().data(), restored.bytes().size(), encoded.bytes().data(),
                                           output.pos),
                           expected_size);
                if (preload) {
                    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source));
                }
                encoded.check_prefix();
                restored.check_prefix();
            }
        }
    }
}

// Purpose: Exercise the public legacy trainer that consumes dictionary merge ranking.
// Inputs: Fixed sample bytes and three selectivity controls, with guarded dictionary capacity.
// Outputs: Requires successful complete dictionaries and real compression/read-back with each result.
TEST_CASE(zstd_legacy_dictionary_merger_consumer) {
    const Samples samples;
    for (const auto selectivity : {0U, 5U, 9U}) {
        GuardedBuffer dictionary(8192U);
        const ZDICT_legacy_params_t parameters{selectivity, {5, 0, 0}};
        const auto result =
            ZDICT_trainFromBuffer_legacy(dictionary.bytes().data(), dictionary.bytes().size(), samples.bytes.data(),
                                         samples.sizes.data(), static_cast<unsigned>(samples.sizes.size()), parameters);
        REQUIRE_TRUE(!ZDICT_isError(result));
        REQUIRE_TRUE(result >= ZDICT_DICTSIZE_MIN && result <= dictionary.bytes().size());
        check_dictionary_readback(dictionary.bytes().first(result), samples);
        dictionary.check_prefix();
    }
}

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

// Purpose: Preserve raw and RLE literal header parsing, placement and failure behavior before decoder refactoring.
// Inputs: Exact synthetic frames cover every header width, small/large extents and both literal kinds.
// Outputs: Requires public-DLL byte-exact decoding and guarded capacity/truncation failures for each frame.
TEST_CASE(zstd_literal_raw_and_rle_header_contracts) {
    for (const auto size : {1U, 2U, 31U, 32U, 255U, 256U, 4095U, 4096U, 65535U, 131072U}) {
        for (const auto width : {1U, 2U, 3U}) {
            if ((width == 1U && size > 31U) || (width == 2U && size > 4095U)) {
                continue;
            }
            for (const auto rle : {false, true}) {
                if (!rle && size == 131072U) {
                    continue;  // The encoded compressed block also needs its header and sequence-count byte.
                }
                std::vector<unsigned char> expected(size);
                for (std::size_t byte = 0; byte < size; ++byte) {
                    expected[byte] = rle ? 0xA7U : static_cast<unsigned char>((byte * 73U + 19U) & 255U);
                }
                const auto frame = make_literal_frame(expected, width, rle);
                require_literal_frame(frame, expected);
                require_literal_bufferless(frame, expected);
                if (width == 3U && size >= 4096U) {
                    require_literal_stream(frame, expected, 17U, 31U);
                    require_literal_stream(frame, expected, 4096U, 4096U);
                }
            }
        }
    }
}

// Purpose: Freeze compressed and repeated Huffman literal decoding before restructuring the production switch.
// Inputs: Three deterministic low-alphabet corpora span four blocks each at compression level five.
// Outputs: Requires actual compressed/repeated literal branches, exact guarded read-back and rejection contracts.
TEST_CASE(zstd_literal_compressed_and_repeat_contracts) {
    std::array<std::size_t, 4> counts{};
    for (auto seed : {0x913579BDU, 0xA76C1203U, 0x72B491E5U}) {
        std::vector<unsigned char> expected(4U * 131072U);
        for (auto& byte : expected) {
            seed ^= seed << 13U;
            seed ^= seed >> 17U;
            seed ^= seed << 5U;
            byte = static_cast<unsigned char>(seed & 31U);
        }
        std::vector<unsigned char> frame(ZSTD_compressBound(expected.size()));
        const auto size = ZSTD_compress(frame.data(), frame.size(), expected.data(), expected.size(), 5);
        REQUIRE_TRUE(!ZSTD_isError(size));
        frame.resize(size);
        count_literal_encodings(frame, counts);
        require_literal_frame(frame, expected);
        require_literal_bufferless(frame, expected);
        require_literal_stream(frame, expected, 17U, 31U);
        require_literal_stream(frame, expected, 4096U, 4096U);
    }
    REQUIRE_TRUE(counts[2] > 0U);
    REQUIRE_TRUE(counts[3] > 0U);
}

// Purpose: Establish checksum and skippable-frame controls for the public bufferless state machine.
// Inputs: Deterministic checksummed modern frames, one corrupted checksum per size, and a bounded skippable frame.
// Outputs: Requires exact read-back, checksum-specific rejection and zero-output skip completion at guard boundaries.
TEST_CASE(zstd_bufferless_checksum_and_skip_contracts) {
    for (const auto size : {1U, 32U, 4096U, 131072U, 524288U}) {
        CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
        REQUIRE_TRUE(encoder != nullptr);
        REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_setParameter(encoder.get(), ZSTD_c_checksumFlag, 1)));
        std::vector<unsigned char> expected(size);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<unsigned char>((index * 73U + index / 31U) & 255U);
        }
        std::vector<unsigned char> frame(ZSTD_compressBound(size));
        const auto encoded =
            ZSTD_compress2(encoder.get(), frame.data(), frame.size(), expected.data(), expected.size());
        REQUIRE_TRUE(!ZSTD_isError(encoded));
        frame.resize(encoded);
        require_literal_bufferless(frame, expected);
        require_literal_stream(frame, expected, 1U, 31U);
        for (const auto magicless : {false, true}) {
            require_stable_stream(frame, expected, magicless, 1U);
            require_stable_stream(frame, expected, magicless, frame.size());
        }
        frame.back() ^= 0x80U;
        GuardedBuffer output(expected.size());
        REQUIRE_EQ(ZSTD_getErrorCode(decode_bufferless(frame, output)), ZSTD_error_checksum_wrong);
    }
    std::vector<unsigned char> skippable;
    append_wire_integer(skippable, ZSTD_MAGIC_SKIPPABLE_START + 7U, 4U);
    append_wire_integer(skippable, 19U, 4U);
    skippable.resize(27U, 0xA7U);
    GuardedBuffer empty(0U);
    REQUIRE_EQ(decode_bufferless(skippable, empty), 0U);
}

// Purpose: Establish direct streaming-compressor stage controls before its production refactor.
// Inputs: Empty, tiny and multi-block deterministic data across buffered/stable input/output and explicit flushes.
// Outputs: Requires complete frame boundaries and exact independent one-shot, bufferless and chunk-stream read-back.
TEST_CASE(zstd_compression_stream_stage_contracts) {
    for (const auto size : {0U, 1U, 4097U, 131073U, 524288U}) {
        std::vector<unsigned char> source(size);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<unsigned char>((index * 73U + index / 31U) & 255U);
        }
        for (const auto stable_input : {false, true}) {
            for (const auto stable_output : {false, true}) {
                for (const auto flush_mid : {false, true}) {
                    if (stable_input && flush_mid) {
                        continue;
                    }
                    const auto frame = compress_stream_frame(source, stable_input, stable_output, flush_mid);
                    REQUIRE_EQ(ZSTD_findFrameCompressedSize(frame.data(), frame.size()), frame.size());
                    GuardedBuffer restored(source.size());
                    REQUIRE_EQ(
                        ZSTD_decompress(restored.bytes().data(), restored.bytes().size(), frame.data(), frame.size()),
                        source.size());
                    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source));
                    restored.check_prefix();
                    REQUIRE_EQ(decode_bufferless(frame, restored), source.size());
                    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), source));
                    require_literal_stream(frame, source, 17U, 31U);
                    require_stable_stream(frame, source, false, 17U);
                }
            }
        }
    }
}

// Purpose: Preserve explicit stream-position errors and valid empty-buffer backpressure/recovery.
// Inputs: Guarded zero-length extents, invalid positions including SIZE_MAX, and nullable zero-length public buffers.
// Outputs: Requires native errors without cursor mutation and exact frame completion after output becomes available.
TEST_CASE(zstd_stream_empty_buffers_and_invalid_positions) {
    GuardedBuffer empty(0U);
    for (const auto position : {std::size_t{1}, std::numeric_limits<std::size_t>::max()}) {
        DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
        REQUIRE_TRUE(decoder != nullptr);
        ZSTD_inBuffer input{empty.bytes().data(), 0U, position};
        ZSTD_outBuffer output{empty.bytes().data(), 0U, 0U};
        REQUIRE_EQ(ZSTD_getErrorCode(ZSTD_decompressStream(decoder.get(), &output, &input)), ZSTD_error_srcSize_wrong);
        REQUIRE_EQ(input.pos, position);
        REQUIRE_EQ(output.pos, 0U);
        input.pos = 0U;
        output.pos = position;
        REQUIRE_EQ(ZSTD_getErrorCode(ZSTD_decompressStream(decoder.get(), &output, &input)),
                   ZSTD_error_dstSize_tooSmall);
        REQUIRE_EQ(input.pos, 0U);
        REQUIRE_EQ(output.pos, position);
        empty.check_prefix();
    }
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    ZSTD_inBuffer no_input{nullptr, 0U, 0U};
    ZSTD_outBuffer no_output{nullptr, 0U, 0U};
    const auto hint = ZSTD_decompressStream(decoder.get(), &no_output, &no_input);
    REQUIRE_TRUE(!ZSTD_isError(hint) && hint > 0U);
    const std::array<unsigned char, 1> expected{0x5CU};
    const auto frame = make_literal_frame(expected, 1U, false);
    ZSTD_inBuffer input{frame.data(), frame.size(), 0U};
    const auto pending = ZSTD_decompressStream(decoder.get(), &no_output, &input);
    REQUIRE_TRUE(!ZSTD_isError(pending) && pending > 0U);
    REQUIRE_EQ(no_output.pos, 0U);
    GuardedBuffer restored(expected.size());
    ZSTD_outBuffer output{restored.bytes().data(), restored.bytes().size(), 0U};
    REQUIRE_EQ(ZSTD_decompressStream(decoder.get(), &output, &input), 0U);
    REQUIRE_EQ(input.pos, frame.size());
    REQUIRE_EQ(output.pos, expected.size());
    REQUIRE_TRUE(std::ranges::equal(restored.bytes(), expected));
    restored.check_prefix();
    CompressionOwner encoder(ZSTD_createCCtx(), ZSTD_freeCCtx);
    REQUIRE_TRUE(encoder != nullptr);
    const auto remaining = ZSTD_compressStream2(encoder.get(), &no_output, &no_input, ZSTD_e_end);
    REQUIRE_TRUE(!ZSTD_isError(remaining) && remaining > 0U);
    REQUIRE_EQ(no_input.pos, 0U);
    REQUIRE_EQ(no_output.pos, 0U);
    GuardedBuffer encoded(ZSTD_compressBound(0U));
    ZSTD_outBuffer destination{encoded.bytes().data(), encoded.bytes().size(), 0U};
    REQUIRE_EQ(ZSTD_compressStream2(encoder.get(), &destination, &no_input, ZSTD_e_end), 0U);
    REQUIRE_EQ(ZSTD_findFrameCompressedSize(encoded.bytes().data(), destination.pos), destination.pos);
    REQUIRE_EQ(ZSTD_decompress(empty.bytes().data(), 0U, encoded.bytes().data(), destination.pos), 0U);
    DecompressionOwner stable(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(stable != nullptr);
    REQUIRE_TRUE(!ZSTD_isError(ZSTD_DCtx_setParameter(stable.get(), ZSTD_d_stableOutBuffer, 1)));
    std::size_t consumed = 0U;
    bool complete = false;
    for (std::size_t calls = 0; calls <= destination.pos + 2U; ++calls) {
        ZSTD_inBuffer chunk{encoded.bytes().data() + consumed, std::min(std::size_t{1}, destination.pos - consumed),
                            0U};
        const auto result = ZSTD_decompressStream(stable.get(), &no_output, &chunk);
        REQUIRE_TRUE(!ZSTD_isError(result));
        consumed += chunk.pos;
        REQUIRE_TRUE(consumed <= destination.pos);
        REQUIRE_EQ(no_output.pos, 0U);
        if (result == 0U) {
            complete = true;
            break;
        }
        REQUIRE_TRUE(chunk.pos > 0U);
    }
    REQUIRE_TRUE(complete);
    REQUIRE_EQ(consumed, destination.pos);
    encoded.check_prefix();
    empty.check_prefix();
}

// Purpose: Reject invalid dictionary initialization before a legacy frame writes caller output.
// Inputs: version is 5-7; guarded malformed entropy dictionaries and a valid literal-only frame.
// Outputs: Requires dictionary_corrupted, unchanged output, and exact read-back after resetting the same decoder.
void require_legacy_dictionary_error(unsigned version) {
    constexpr std::array<unsigned char, 3> expected{0x41U, 0x42U, 0x43U};
    const auto frame = make_legacy_literal_frame(version, expected, 1U, false);
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    const auto minimum = version == 7U ? 8U : 4U;
    for (const auto size : {minimum, minimum + 1U, 16U}) {
        GuardedBuffer dictionary(size);
        std::ranges::fill(dictionary.bytes(), 0U);
        dictionary.bytes()[0] = static_cast<unsigned char>(0x30U + version);
        dictionary.bytes()[1] = 0xA4U;
        dictionary.bytes()[2] = 0x30U;
        dictionary.bytes()[3] = 0xECU;
        GuardedBuffer output(expected.size());
        std::ranges::fill(output.bytes(), 0xA5U);
        const auto result = ZSTD_decompress_usingDict(decoder.get(), output.bytes().data(), output.bytes().size(),
                                                      frame.data(), frame.size(), dictionary.bytes().data(), size);
        REQUIRE_TRUE(ZSTD_isError(result));
        REQUIRE_EQ(ZSTD_getErrorCode(result), ZSTD_error_dictionary_corrupted);
        REQUIRE_TRUE(std::ranges::all_of(output.bytes(), [](auto byte) { return byte == 0xA5U; }));
        REQUIRE_EQ(ZSTD_decompress_usingDict(decoder.get(), output.bytes().data(), output.bytes().size(), frame.data(),
                                             frame.size(), nullptr, 0U),
                   expected.size());
        REQUIRE_TRUE(std::ranges::equal(output.bytes(), expected));
        dictionary.check_prefix();
        output.check_prefix();
    }
}

// Purpose: Cover public v0.5 dictionary errors; no inputs, requires rejection before output and a valid retry.
TEST_CASE(zstd_legacy_v05_dictionary_error_propagation) {
    require_legacy_dictionary_error(5U);
}

// Purpose: Cover public v0.6 dictionary errors; no inputs, requires rejection before output and a valid retry.
TEST_CASE(zstd_legacy_v06_dictionary_error_propagation) {
    require_legacy_dictionary_error(6U);
}

// Purpose: Cover public v0.7 dictionary errors; no inputs, requires rejection before output and a valid retry.
TEST_CASE(zstd_legacy_v07_dictionary_error_propagation) {
    require_legacy_dictionary_error(7U);
}

// Purpose: Preserve short raw dictionaries without reading a four-byte marker outside their initialized extent.
// Inputs: version is 5-7; each 1-3 byte dictionary ends directly at an unreadable page and remains borrowed per call.
// Outputs: Requires exact public-DLL decoding and unchanged dictionary/capacity guards without host error dialogs.
void require_legacy_short_dictionary(unsigned version) {
    ScopedThreadErrorMode error_mode;
    constexpr std::array<unsigned char, 3> expected{0x41U, 0x42U, 0x43U};
    const auto frame = make_legacy_literal_frame(version, expected, 1U, false);
    DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    REQUIRE_TRUE(decoder != nullptr);
    for (const auto size : {1U, 2U, 3U}) {
        GuardedBuffer dictionary(size);
        std::ranges::fill(dictionary.bytes(), 0xA5U);
        GuardedBuffer output(expected.size());
        REQUIRE_EQ(ZSTD_decompress_usingDict(decoder.get(), output.bytes().data(), output.bytes().size(), frame.data(),
                                             frame.size(), dictionary.bytes().data(), size),
                   expected.size());
        REQUIRE_TRUE(std::ranges::equal(output.bytes(), expected));
        REQUIRE_TRUE(std::ranges::all_of(dictionary.bytes(), [](auto byte) { return byte == 0xA5U; }));
        dictionary.check_prefix();
        output.check_prefix();
    }
}

// Purpose: Cover v0.5 short raw dictionary extents; no inputs, requires guarded successful decoding.
TEST_CASE(zstd_legacy_v05_short_raw_dictionary) {
    require_legacy_short_dictionary(5U);
}

// Purpose: Cover v0.6 short raw dictionary extents; no inputs, requires guarded successful decoding.
TEST_CASE(zstd_legacy_v06_short_raw_dictionary) {
    require_legacy_short_dictionary(6U);
}

// Purpose: Retain v0.7's existing short dictionary contract; no inputs, requires guarded successful decoding.
TEST_CASE(zstd_legacy_v07_short_raw_dictionary) {
    require_legacy_short_dictionary(7U);
}

// Purpose: Establish pinned legacy frame, streaming and dictionary-lifetime controls before decoder refactoring.
// Inputs: Exact upstream version 4-8 frames and expected bytes; a guarded raw dictionary remains live per call.
// Outputs: Requires supported frame/read-back contracts and retains version-four rejection.
TEST_CASE(zstd_legacy_golden_frame_contracts) {
    const auto expected = std::span(superzip_test::kLegacyExpected);
    for (const auto& extent : superzip_test::kLegacyFrames) {
        const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
        if (extent.version == 4U) {
            GuardedBuffer output(expected.size());
            REQUIRE_TRUE(ZSTD_isError(
                ZSTD_decompress(output.bytes().data(), output.bytes().size(), frame.data(), frame.size())));
            output.check_prefix();
            continue;
        }
        REQUIRE_EQ(ZSTD_findFrameCompressedSize(frame.data(), frame.size()), frame.size());
        require_literal_frame(frame, expected);
        require_literal_stream(frame, expected, 17U, 31U);
        require_literal_stream(frame, expected, 4096U, 4096U);
        DecompressionOwner decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
        REQUIRE_TRUE(decoder != nullptr);
        GuardedBuffer dictionary(512U);
        std::ranges::fill(dictionary.bytes(), 0xA5U);
        GuardedBuffer output(expected.size());
        REQUIRE_EQ(ZSTD_decompress_usingDict(decoder.get(), output.bytes().data(), output.bytes().size(), frame.data(),
                                             frame.size(), dictionary.bytes().data(), dictionary.bytes().size()),
                   expected.size());
        REQUIRE_TRUE(std::ranges::equal(output.bytes(), expected));
        dictionary.check_prefix();
        output.check_prefix();
    }
}

// Purpose: Cover every legacy raw/RLE header width and reject oversized literals through the production DLL.
// Inputs: Three supported legacy versions, deterministic literals and a malformed size just above the block limit.
// Outputs: Requires exact guarded read-back, streaming, capacity/truncation errors and bounded malformed failure.
TEST_CASE(zstd_legacy_literal_header_contracts) {
    for (const auto version : {5U, 6U, 7U}) {
        for (const auto size : {1U, 2U, 31U, 32U, 255U, 256U, 4095U, 4096U, 65535U, 131072U}) {
            for (const auto width : {1U, 2U, 3U}) {
                if ((width == 1U && size > 31U) || (width == 2U && size > 4095U)) {
                    continue;
                }
                for (const auto rle : {false, true}) {
                    if (!rle && size == 131072U) {
                        continue;
                    }
                    std::vector<unsigned char> expected(size);
                    for (std::size_t byte = 0; byte < expected.size(); ++byte) {
                        expected[byte] = rle ? 0xA7U : static_cast<unsigned char>((byte * 73U + 19U) & 255U);
                    }
                    auto frame = make_legacy_literal_frame(version, expected, width, rle);
                    require_literal_frame(frame, expected);
                    if (width == 3U && size >= 4096U) {
                        require_literal_stream(frame, expected, 17U, 31U);
                        require_literal_stream(frame, expected, 4096U, 4096U);
                    }
                    if (size == 131072U) {
                        // Increment the big-endian literal-size field from 128 KiB to 128 KiB + 1.
                        frame[(version == 7U ? 9U : 8U) + 2U] = 1U;
                        GuardedBuffer input(frame.size());
                        std::ranges::copy(frame, input.bytes().begin());
                        GuardedBuffer output(4096U);
                        REQUIRE_TRUE(ZSTD_isError(ZSTD_decompress(output.bytes().data(), output.bytes().size(),
                                                                  input.bytes().data(), input.bytes().size())));
                        input.check_prefix();
                        output.check_prefix();
                    }
                }
            }
        }
    }
}
