#include "test_util.hpp"
#include "test_source_session.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/result.hpp"
#include "core/windows_handle.hpp"
#include "gzip/gzip_stream.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

class StreamFixtureByteLock final {
  public:
    // Purpose: Retain a stable native lock owner before constructing the stream that will later be refused.
    // Inputs: None. Outputs: Inactive, noncopyable owner; acquisition occurs explicitly after stream construction.
    StreamFixtureByteLock() = default;
    StreamFixtureByteLock(const StreamFixtureByteLock&) = delete;
    StreamFixtureByteLock& operator=(const StreamFixtureByteLock&) = delete;

    // Purpose: Release the test-owned lock on every assertion/error path without throwing during unwinding.
    // Inputs: Owned handle/range only. Outputs: Explicit best-effort unlock followed by unconditional handle release.
    ~StreamFixtureByteLock() {
        if (locked_)
            UnlockFileEx(handle_.get(), 0, length_, 0, &position_);
    }

    // Purpose: Refuse a real existing stream's next overlapping native I/O through a compatible second read handle.
    // Inputs: Owned regular file, bounded low-32-bit offset and nonzero range, possibly beyond current EOF.
    // Outputs: Immediate exclusive byte lock, with all acquired resources owned before assertions can throw.
    void acquire(const std::filesystem::path& path, DWORD offset, DWORD length) {
        REQUIRE_TRUE(!handle_ && !locked_ && length > 0U);
        handle_.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE_TRUE(handle_ && handle_.get() != INVALID_HANDLE_VALUE);
        position_.Offset = offset;
        length_ = length;
        locked_ = LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, length_, 0,
                             &position_) != FALSE;
        REQUIRE_TRUE(locked_);
    }

    // Purpose: Prove refusal did not mutate the source, using the handle that actually owns the exclusive lock.
    // Inputs: Independently retained complete wire bytes. Outputs: Exact native file size and complete byte readback.
    void require_source_bytes(std::span<const unsigned char> expected) const {
        REQUIRE_TRUE(locked_ && expected.size() < 1024U * 1024U);
        LARGE_INTEGER size{};
        REQUIRE_TRUE(GetFileSizeEx(handle_.get(), &size) != FALSE);
        REQUIRE_EQ(static_cast<std::uint64_t>(size.QuadPart), expected.size());
        LARGE_INTEGER beginning{};
        REQUIRE_TRUE(SetFilePointerEx(handle_.get(), beginning, nullptr, FILE_BEGIN) != FALSE);
        std::vector<unsigned char> actual(expected.size());
        DWORD count = 0;
        REQUIRE_TRUE(ReadFile(handle_.get(), actual.data(), static_cast<DWORD>(actual.size()), &count, nullptr) !=
                     FALSE);
        REQUIRE_EQ(count, expected.size());
        REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), expected.begin()));
    }

    // Purpose: End the OS refusal before testing the decoder's retained terminal error or opening a replacement owner.
    // Inputs: Active owned lock. Outputs: Explicitly unlocked and closed handle; failed unlock remains cleanup-owned.
    void release() {
        REQUIRE_TRUE(locked_);
        REQUIRE_TRUE(UnlockFileEx(handle_.get(), 0, length_, 0, &position_) != FALSE);
        locked_ = false;
        handle_.reset();
    }

  private:
    flitzzip::windows_detail::HandleOwner handle_;
    OVERLAPPED position_{};
    DWORD length_ = 0;
    bool locked_ = false;
};

// Purpose: Detect leaked codec/source/file owners through actual native sharing admission.
// Inputs: Owned regular file after all intended stream and lock owners have left scope.
// Outputs: A real exclusive read handle succeeds and closes before the caller's next operation.
void require_stream_file_owners_released(const std::filesystem::path& path) {
    const flitzzip::windows_detail::HandleOwner probe(
        CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE_TRUE(probe && probe.get() != INVALID_HANDLE_VALUE);
}

// Purpose: Construct a fixed, bounded high-entropy caller payload without using a codec's encoder or RNG state.
// Inputs: Requested byte count below one MiB. Outputs: Reproducible xorshift32 bytes from a literal seed.
std::string stream_lock_payload(std::size_t size) {
    REQUIRE_TRUE(size < 1024U * 1024U);
    std::uint32_t state = 0x12345678U;
    std::string result(size, '\0');
    for (auto& byte : result) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<char>(state & 255U);
    }
    return result;
}

// Purpose: Build a complete large Gzip frame independently, with two stored DEFLATE blocks and a bitwise CRC oracle.
// Inputs: Exactly 69632 known bytes. Outputs: 69660 wire bytes with a late compressed byte beyond the first64KiB read.
std::vector<unsigned char> stream_lock_gzip_wire(std::string_view payload) {
    REQUIRE_EQ(payload.size(), 69632U);
    std::vector<unsigned char> wire{31U, 139U, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 255U};
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : payload) {
        crc ^= static_cast<unsigned char>(value);
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    for (std::size_t offset = 0; offset < payload.size();) {
        const auto count = std::min<std::size_t>(65535U, payload.size() - offset);
        wire.push_back(offset + count == payload.size() ? 1U : 0U);
        const auto inverse = static_cast<unsigned>(count) ^ 65535U;
        wire.insert(wire.end(),
                    {static_cast<unsigned char>(count & 255U), static_cast<unsigned char>(count >> 8U),
                     static_cast<unsigned char>(inverse & 255U), static_cast<unsigned char>(inverse >> 8U)});
        wire.insert(wire.end(), payload.begin() + offset, payload.begin() + offset + count);
        offset += count;
    }
    for (const std::uint32_t field : {~crc, static_cast<std::uint32_t>(payload.size())}) {
        for (unsigned byte = 0; byte < 4U; ++byte)
            wire.push_back(static_cast<unsigned char>((field >> (byte * 8U)) & 255U));
    }
    REQUIRE_EQ(wire.size(), 69660U);
    return wire;
}

// Purpose: Retain an independent complete Bzip2 input without relying on the product encoder.
// Inputs: None. Outputs: Python stdlib bz2 level-one wire for the literal five bytes "hello".
std::vector<unsigned char> stream_lock_bzip2_wire() {
    return {0x42U, 0x5aU, 0x68U, 0x31U, 0x31U, 0x41U, 0x59U, 0x26U, 0x53U, 0x59U, 0x19U, 0x31U, 0x65U, 0x3dU,
            0x00U, 0x00U, 0x00U, 0x81U, 0x00U, 0x02U, 0x44U, 0xa0U, 0x00U, 0x21U, 0x9aU, 0x68U, 0x33U, 0x4dU,
            0x07U, 0x33U, 0x8bU, 0xb9U, 0x22U, 0x9cU, 0x28U, 0x48U, 0x0cU, 0x98U, 0xb2U, 0x9eU, 0x80U};
}

// Purpose: Write only independently retained bounded input bytes, closing the writer before decoder construction.
// Inputs: Owned regular-file pathname and complete wire. Outputs: Exact binary file with no overlapping writer owner.
void write_stream_lock_wire(const std::filesystem::path& path, std::span<const unsigned char> wire) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
    output.close();
}

// Purpose: Require an actual product-domain native read refusal rather than ordinary EOF or an unrelated error.
// Inputs: Synchronous stream action and the independently selected codec diagnostic.
// Outputs: Exact ArchiveError or assertion failure; no broader exception is accepted as success.
template <typename Operation> void require_stream_native_error(Operation&& operation, std::string_view expected) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string_view(error.what()), expected);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Qualify a complete independent wire through the public decoder with fixed plaintext and accounting.
// Inputs: Closed owned archive and independently retained payload. Outputs: Exact readback and native-owner release.
template <typename Input>
void require_stream_lock_valid_read(const std::filesystem::path& path, std::string_view payload) {
    {
        Input input(path);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), payload);
        input.finish();
        REQUIRE_EQ(input.output_bytes(), payload.size());
    }
    require_stream_file_owners_released(path);
}

// Purpose: Turn actual read-lock refusal into a terminal decoder failure while preserving source and caller bytes.
// Inputs: Independent complete wire, literal payload, lock offset and optional one-byte valid prefix.
// Outputs: No clean EOF/success, guarded accepted prefix, sticky failure after unlock and exact new-owner recovery.
template <typename Input>
void require_stream_native_read_refusal(const char* label, const std::vector<unsigned char>& wire,
                                        std::string_view payload, DWORD lock_offset, bool read_prefix) {
    const auto root = test_temp_dir(label);
    const auto path = root / "frame";
    write_stream_lock_wire(path, wire);
    require_stream_lock_valid_read<Input>(path, payload);
    StreamFixtureByteLock blocker;
    {
        Input input(path);
        blocker.acquire(path, lock_offset, 1U);
        std::array<char, 11> destination{};
        destination.fill('Z');
        auto accepted = destination;
        const auto expected = std::is_same_v<Input, flitzzip::GzipInputStream>
                                  ? "Gzip compressed payload is truncated"
                                  : "Bzip2 compressed payload is truncated";
        if (read_prefix) {
            input.read(destination.data() + 5, 1);
            REQUIRE_EQ(input.gcount(), 1);
            accepted[5] = payload.front();
            REQUIRE_EQ(destination, accepted);
            REQUIRE_TRUE(input.good());
            require_stream_native_error([&] { input.finish(); }, expected);
        } else {
            require_stream_native_error([&] { input.read(destination.data() + 5, 1); }, expected);
            REQUIRE_TRUE(input.bad());
        }
        REQUIRE_TRUE(!input.eof());
        REQUIRE_EQ(destination, accepted);
        REQUIRE_TRUE(input.output_bytes() <= (read_prefix ? 65536U : 0U));
        blocker.require_source_bytes(wire);
        blocker.release();
        const auto produced = input.output_bytes();
        for (int attempt = 0; attempt < 2; ++attempt) {
            input.clear();
            require_stream_native_error([&] { input.read(destination.data() + 5, 1); }, expected);
            REQUIRE_TRUE(input.bad() && !input.eof());
            REQUIRE_EQ(destination, accepted);
            input.clear();
            require_stream_native_error([&] { input.finish(); }, expected);
            REQUIRE_EQ(input.output_bytes(), produced);
        }
    }
    require_stream_file_owners_released(path);
    REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(path),
               std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
    require_stream_lock_valid_read<Input>(path, payload);
}

// Purpose: Require actual sink refusal without pretending an already-open output stream is a publication transaction.
// Inputs: One caller write or finalization action and a codec label. Outputs: An exact native stream-write/close error.
template <typename Operation> void require_stream_sink_refusal(Operation&& operation, std::string_view codec) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::ArchiveError& error) {
        const std::string message(error.what());
        REQUIRE_TRUE(message == "failed to write " + std::string(codec) + " stream" ||
                     message == "failed to finalize " + std::string(codec) + " stream");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Exercise real synchronous sink refusal during streaming and finalization with cleanup and valid reuse.
// Inputs: Existing public encoder/decoder pair, codec and its only permitted constructor-written prefix.
// Outputs: Unchanged caller/sibling bytes, bounded unverified sink prefix, released owners and fresh exact encoding.
template <typename Output, typename Input>
void require_stream_native_sink_refusal(const char* codec, std::span<const unsigned char> constructor_prefix) {
    const auto root = test_temp_dir(std::string("stream-native-sink-") + codec);
    const auto sibling = root / "sibling.txt";
    std::ofstream(sibling, std::ios::binary) << "unrelated caller owner";
    const auto independent = root / "independent-control";
    if constexpr (std::is_same_v<Input, flitzzip::GzipInputStream>) {
        const auto known = stream_lock_payload(69632U);
        write_stream_lock_wire(independent, stream_lock_gzip_wire(known));
        require_stream_lock_valid_read<Input>(independent, known);
    } else {
        write_stream_lock_wire(independent, stream_lock_bzip2_wire());
        require_stream_lock_valid_read<Input>(independent, "hello");
    }
    for (const bool finalize : {false, true}) {
        const auto path = root / (finalize ? "close-frame" : "write-frame");
        const auto payload = finalize ? std::string("hello") : stream_lock_payload(196608U);
        const auto original = payload;
        {
            Output control(path, 1);
            control.exceptions(std::ios::badbit | std::ios::failbit);
            control.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            control.close();
        }
        require_stream_lock_valid_read<Input>(path, payload);
        StreamFixtureByteLock blocker;
        {
            Output output(path, 1);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            blocker.acquire(path, 0U, 1024U * 1024U);
            if (finalize) {
                output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
                REQUIRE_TRUE(output.good());
                require_stream_sink_refusal([&] { output.close(); }, codec);
            } else {
                require_stream_sink_refusal(
                    [&] { output.write(payload.data(), static_cast<std::streamsize>(payload.size())); }, codec);
                REQUIRE_TRUE(output.bad());
            }
            REQUIRE_EQ(payload, original);
            REQUIRE_TRUE(output.input_bytes() <= payload.size());
        }  // The lock remains held through actual encoder/file destructor cleanup.
        blocker.release();
        require_stream_file_owners_released(path);
        const auto partial = flitzzip_test::source_session_fixture_bytes(path);
        REQUIRE_TRUE(partial.size() <= constructor_prefix.size());
        REQUIRE_TRUE(
            std::equal(partial.begin(), partial.end(), constructor_prefix.begin(),
                       [](char left, unsigned char right) { return static_cast<unsigned char>(left) == right; }));
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(sibling), "unrelated caller owner");
        {
            Output replacement(path, 1);
            replacement.exceptions(std::ios::badbit | std::ios::failbit);
            replacement.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            replacement.close();
        }
        require_stream_file_owners_released(path);
        require_stream_lock_valid_read<Input>(path, payload);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(sibling), "unrelated caller owner");
        REQUIRE_EQ(payload, original);
    }
}

}  // namespace

// Purpose: Reject actual late source I/O refusal even when an independent Gzip member has a valid decoded prefix.
// Inputs: Two stored DEFLATE blocks and a lock on their last compressed byte, acquired after stream construction.
// Outputs: Source preservation, no clean completion, retained terminal failure after unlock and exact new-reader reuse.
TEST_CASE(compression_stream_gzip_native_read_lock_failure_is_terminal) {
    const auto payload = stream_lock_payload(69632U);
    const auto wire = stream_lock_gzip_wire(payload);
    require_stream_native_read_refusal<flitzzip::GzipInputStream>("gzip-native-read-lock", wire, payload,
                                                                  static_cast<DWORD>(wire.size() - 9U), true);
}

// Purpose: Reject a real compressed-input read refusal without treating its independent Bzip2 member as clean EOF.
// Inputs: Complete stdlib-written "hello" member and a compatible lock acquired after decoder construction.
// Outputs: Unchanged source/caller bytes, sticky native read failure after unlock, released owners and valid reuse.
TEST_CASE(compression_stream_bzip2_native_read_lock_failure_is_terminal) {
    require_stream_native_read_refusal<flitzzip::Bzip2InputStream>("bzip2-native-read-lock", stream_lock_bzip2_wire(),
                                                                   "hello", 0U, false);
}

// Purpose: Release Gzip encoder/file state after actual native write and close failures, retaining caller input.
// Inputs: Per-instance beyond-EOF lock and complete bounded writes; Gzip may already have written its fixed header.
// Outputs: Only a bounded unverified header prefix remains, then same-path encoding decodes exactly after unlock.
TEST_CASE(compression_stream_gzip_native_sink_lock_failure_releases_owners) {
    constexpr std::array<unsigned char, 10> header{31U, 139U, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 255U};
    require_stream_native_sink_refusal<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("Gzip", header);
}

// Purpose: Release Bzip2 encoder/file state after actual native write and close failures, retaining caller input.
// Inputs: Per-instance beyond-EOF lock and complete bounded writes; this encoder writes no constructor prefix.
// Outputs: No sink payload is accepted, native owners release and same-path encoding decodes exactly after unlock.
TEST_CASE(compression_stream_bzip2_native_sink_lock_failure_releases_owners) {
    require_stream_native_sink_refusal<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>("Bzip2", {});
}
