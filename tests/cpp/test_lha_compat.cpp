#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "core/windows_handle.hpp"
#include "lha/lha_adapter.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include "lha_reader.h"
#include "../../third_party/lhasa/lib/lha_input_stream.h"
}

namespace {

constexpr std::array<unsigned char, 206> kNestedLhaFixture{
    0x19, 0x8E, 0x2D, 0x6C, 0x68, 0x64, 0x2D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE9, 0xA3, 0x98, 0x40,
    0x20, 0x01, 0x00, 0x00, 0x00, 0x55, 0x05, 0x00, 0x50, 0xC0, 0x41, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x0A,
    0x00, 0x02, 0x73, 0x75, 0x62, 0x64, 0x69, 0x72, 0xFF, 0x07, 0x00, 0x54, 0x87, 0xFF, 0x96, 0x4F, 0x00, 0x00, 0x19,
    0x96, 0x2D, 0x6C, 0x68, 0x64, 0x2D, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE9, 0xA3, 0x98, 0x40, 0x20,
    0x01, 0x00, 0x00, 0x00, 0x55, 0x05, 0x00, 0x50, 0x6D, 0x41, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x12, 0x00,
    0x02, 0x73, 0x75, 0x62, 0x64, 0x69, 0x72, 0xFF, 0x73, 0x75, 0x62, 0x64, 0x69, 0x72, 0x32, 0xFF, 0x07, 0x00, 0x54,
    0x87, 0xFF, 0x96, 0x4F, 0x00, 0x00, 0x22, 0x2D, 0x2D, 0x6C, 0x68, 0x30, 0x2D, 0x31, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x20, 0x01, 0x09, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x2E, 0x74, 0x78, 0x74, 0x78,
    0x97, 0x55, 0x05, 0x00, 0x50, 0xA4, 0x81, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x12, 0x00, 0x02, 0x73, 0x75,
    0x62, 0x64, 0x69, 0x72, 0xFF, 0x73, 0x75, 0x62, 0x64, 0x69, 0x72, 0x32, 0xFF, 0x07, 0x00, 0x54, 0x00, 0x3B, 0x3D,
    0x4B, 0x00, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x0A, 0x00,
};

constexpr std::array<unsigned char, 118> kAbsolutePathLhaFixture{
    0x2A, 0xA7, 0x2D, 0x6C, 0x68, 0x30, 0x2D, 0x49, 0x00, 0x00, 0x00, 0x2E, 0x00, 0x00, 0x00, 0xB3, 0xAA,
    0x85, 0x40, 0x20, 0x01, 0x11, 0x61, 0x62, 0x73, 0x6F, 0x6C, 0x75, 0x74, 0x65, 0x5F, 0x70, 0x61, 0x74,
    0x68, 0x2E, 0x74, 0x78, 0x74, 0xC0, 0x6B, 0x55, 0x05, 0x00, 0x50, 0xA4, 0x81, 0x07, 0x00, 0x51, 0xE8,
    0x03, 0xE8, 0x03, 0x08, 0x00, 0x02, 0xFF, 0x74, 0x6D, 0x70, 0xFF, 0x07, 0x00, 0x54, 0xD2, 0xFE, 0x7D,
    0x4F, 0x00, 0x00, 0x54, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x61, 0x20, 0x66, 0x69, 0x6C, 0x65,
    0x20, 0x74, 0x68, 0x61, 0x74, 0x20, 0x68, 0x61, 0x73, 0x20, 0x61, 0x6E, 0x20, 0x61, 0x62, 0x73, 0x6F,
    0x6C, 0x75, 0x74, 0x65, 0x20, 0x66, 0x69, 0x6C, 0x65, 0x6E, 0x61, 0x6D, 0x65, 0x2E, 0x0A, 0x00,
};

constexpr std::array<unsigned char, 161> kDotDotLhaFixture{
    0x22, 0x09, 0x2D, 0x6C, 0x68, 0x30, 0x2D, 0x26, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x58, 0xA2, 0x3D,
    0x42, 0x20, 0x01, 0x09, 0x65, 0x76, 0x69, 0x6C, 0x31, 0x2E, 0x74, 0x78, 0x74, 0xD2, 0x3A, 0x55, 0x05, 0x00,
    0x50, 0xA4, 0x81, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x06, 0x00, 0x02, 0x2E, 0x2E, 0xFF, 0x07, 0x00,
    0x54, 0xA8, 0x2E, 0x08, 0x51, 0x00, 0x00, 0x74, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x62, 0x61, 0x64,
    0x2E, 0x0A, 0x22, 0xB5, 0x2D, 0x6C, 0x68, 0x30, 0x2D, 0x32, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x91,
    0xA2, 0x3D, 0x42, 0x20, 0x01, 0x09, 0x65, 0x76, 0x69, 0x6C, 0x32, 0x2E, 0x74, 0x78, 0x74, 0x30, 0x3D, 0x55,
    0x05, 0x00, 0x50, 0xA4, 0x81, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x0D, 0x00, 0x02, 0x66, 0x6F, 0x6F,
    0xFF, 0x2E, 0x2E, 0xFF, 0x2E, 0x2E, 0xFF, 0x07, 0x00, 0x54, 0x13, 0x2F, 0x08, 0x51, 0x00, 0x00, 0x74, 0x68,
    0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x61, 0x6C, 0x73, 0x6F, 0x20, 0x62, 0x61, 0x64, 0x2E, 0x0A, 0x00,
};

constexpr std::array<unsigned char, 127> kSymlinkLhaFixture{
    0x28, 0x91, 0x2D, 0x6C, 0x68, 0x64, 0x2D, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0x9F, 0x3D, 0x42,
    0x20, 0x01, 0x0F, 0x66, 0x6F, 0x6F, 0x2E, 0x74, 0x78, 0x74, 0x7C, 0x62, 0x61, 0x72, 0x2E, 0x74, 0x78, 0x74, 0x00,
    0x00, 0x55, 0x05, 0x00, 0x50, 0xFF, 0xA1, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x07, 0x00, 0x54, 0xB3, 0x29,
    0x08, 0x51, 0x00, 0x00, 0x20, 0x4E, 0x2D, 0x6C, 0x68, 0x30, 0x2D, 0x1F, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
    0x44, 0x9F, 0x3D, 0x42, 0x20, 0x01, 0x07, 0x66, 0x6F, 0x6F, 0x2E, 0x74, 0x78, 0x74, 0x78, 0x97, 0x55, 0x05, 0x00,
    0x50, 0xA4, 0x81, 0x07, 0x00, 0x51, 0xE8, 0x03, 0xE8, 0x03, 0x07, 0x00, 0x54, 0xD0, 0x29, 0x08, 0x51, 0x00, 0x00,
    0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x77, 0x6F, 0x72, 0x6C, 0x64, 0x0A, 0x00,
};

constexpr std::array<unsigned char, 73> kUnsignedTimestampLhaFixture{
    0x6E, 0x2A, 0x2A, 0x00, 0x2D, 0x6C, 0x68, 0x64, 0x2D, 0x00, 0x00, 0x1C, 0x00, 0x91, 0x00, 0x2A, 0x00, 0x2D, 0x6C,
    0x68, 0x64, 0x2D, 0x00, 0x00, 0x1C, 0x00, 0x6F, 0xFF, 0x00, 0xFB, 0xFF, 0xF7, 0xFF, 0x0E, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x51, 0x55, 0x5A, 0x33, 0x40, 0x36, 0x50, 0x32, 0x35, 0x33, 0x5E, 0x31, 0x6E, 0x6E, 0xF5, 0xF5,
    0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5, 0x4E, 0x2E, 0x74, 0x78, 0x74, 0x6E, 0x6E, 0x00,
};

constexpr std::array<unsigned char, 45> kPm2CopyTableOverreadFixture{
    0x17, 0xEE, 0x2D, 0x70, 0x6D, 0x32, 0x2D, 0x13, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x41, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Purpose: Write an embedded LHA fixture to disk.
// Inputs: `archive` is the destination and `bytes` are exact archive bytes.
// Outputs: Creates or replaces a small LHA fixture; propagates write and close failures.
void write_lha_fixture(const std::filesystem::path& archive, std::span<const unsigned char> bytes) {
    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

// Purpose: Read a full text file for extraction equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory tree to inspect.
// Outputs: Returns the number of regular files; missing roots count as zero.
std::uint64_t count_regular_files(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

// Purpose: Build independent stored LHA members covering the supported header encodings.
// Inputs: `level` selects level 0, 2, or 3; `wide` selects a level-2 length with a zero low byte.
// Outputs: Returns a complete member without an archive terminator, containing the known CRC-correct payload.
std::vector<unsigned char> stored_lha_member(unsigned char level, bool wide = false) {
    const std::size_t header_size = level == 0 ? 32U : (level == 3 ? 45U : (wide ? 256U : 37U));
    std::vector<unsigned char> bytes(header_size, 0);
    constexpr std::array<unsigned char, 5> method{'-', 'l', 'h', '0', '-'};
    constexpr std::array<unsigned char, 8> name{'f', 'i', 'l', 'e', '.', 't', 'x', 't'};
    constexpr std::array<unsigned char, 12> payload{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '\n'};
    std::copy(method.begin(), method.end(), bytes.begin() + 2);
    bytes[7] = bytes[11] = static_cast<unsigned char>(payload.size());
    bytes[20] = level;
    if (level == 0) {
        bytes[0] = static_cast<unsigned char>(header_size - 2);
        bytes[19] = 0x20;
        bytes[21] = static_cast<unsigned char>(name.size());
        std::copy(name.begin(), name.end(), bytes.begin() + 22);
        bytes[30] = 0x78;
        bytes[31] = 0x97;
        unsigned int checksum = 0;
        for (std::size_t index = 2; index < header_size; ++index) {
            checksum += bytes[index];
        }
        bytes[1] = static_cast<unsigned char>(checksum);
    } else {
        bytes[0] = level == 3 ? 4 : static_cast<unsigned char>(header_size & 0xffU);
        bytes[1] = level == 3 ? 0 : static_cast<unsigned char>(header_size >> 8U);
        bytes[21] = 0x78;
        bytes[22] = 0x97;
        bytes[23] = 'U';
        if (level == 3) {
            bytes[24] = static_cast<unsigned char>(header_size);
            bytes[28] = 13;
            bytes[32] = 1;
            std::copy(name.begin(), name.end(), bytes.begin() + 33);
        } else {
            bytes[24] = 11;
            bytes[26] = 1;
            std::copy(name.begin(), name.end(), bytes.begin() + 27);
        }
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

// Purpose: Independently frame stored level-zero members with arbitrary bounded payloads for real read failures.
// Inputs: Exactly eight ASCII name bytes and at most 8160 payload bytes. Outputs: Header, bitwise ARC CRC and payload.
std::vector<unsigned char> read_failure_lha_member(std::string_view name, std::string_view payload) {
    REQUIRE_EQ(name.size(), 8U);
    REQUIRE_TRUE(payload.size() <= 8160U);
    std::uint16_t crc = 0;
    for (const auto value : payload) {
        crc ^= static_cast<unsigned char>(value);
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = static_cast<std::uint16_t>((crc >> 1U) ^ ((crc & 1U) ? 0xA001U : 0U));
    }
    std::vector<unsigned char> bytes(32, 0);
    bytes[0] = 30;
    constexpr std::string_view method = "-lh0-";
    std::copy(method.begin(), method.end(), bytes.begin() + 2);
    for (unsigned shift = 0; shift < 4U; ++shift) {
        bytes[7U + shift] = bytes[11U + shift] = static_cast<unsigned char>((payload.size() >> (shift * 8U)) & 255U);
    }
    bytes[19] = 0x20;
    bytes[21] = 8;
    std::copy(name.begin(), name.end(), bytes.begin() + 22);
    bytes[30] = static_cast<unsigned char>(crc & 255U);
    bytes[31] = static_cast<unsigned char>(crc >> 8U);
    unsigned checksum = 0;
    for (std::size_t index = 2; index < bytes.size(); ++index)
        checksum += bytes[index];
    bytes[1] = static_cast<unsigned char>(checksum & 255U);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

class LhaFixtureReadLock {
  public:
    // Purpose: Refuse a later archive read on a separate compatible handle without mutating source bytes.
    // Inputs: Owned archive path and bounded byte offset. Outputs: An immediate one-byte lock or a test failure.
    LhaFixtureReadLock(const std::filesystem::path& path, DWORD offset)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        REQUIRE_TRUE(handle_ && handle_.get() != INVALID_HANDLE_VALUE);
        position_.Offset = offset;
        locked_ = LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position_) !=
                  FALSE;
        REQUIRE_TRUE(locked_);
    }

    // Purpose: Release the owned read refusal on all assertion and extraction exits.
    // Inputs: The owned handle only. Outputs: Unlocks before the handle owner closes; never throws during unwinding.
    ~LhaFixtureReadLock() {
        if (locked_)
            UnlockFileEx(handle_.get(), 0, 1, 0, &position_);
    }

  private:
    flitzzip::windows_detail::HandleOwner handle_;
    OVERLAPPED position_{};
    bool locked_ = false;
};

// Purpose: Replace name/path extensions in the independently framed level-two stored member.
// Inputs: Bounded filename and optional raw directory bytes. Outputs: Member with known payload/CRC and exact lengths.
std::vector<unsigned char> named_level_two_lha_member(std::string_view name, std::string_view directory = {}) {
    REQUIRE_TRUE(name.size() <= 4096U);
    REQUIRE_TRUE(directory.size() <= flitzzip::kMaxArchivePathBytes + 1U);
    const auto original = stored_lha_member(2);
    std::vector<unsigned char> bytes(original.begin(), original.begin() + 24);
    const std::pair<unsigned char, std::string_view> fields[] = {{2, directory}, {1, name}};
    for (const auto& [type, data] : fields) {
        if (type == 2 && data.empty()) {
            continue;
        }
        const auto extension_size = data.size() + 3U;
        bytes.push_back(static_cast<unsigned char>(extension_size & 0xFFU));
        bytes.push_back(static_cast<unsigned char>(extension_size >> 8U));
        bytes.push_back(type);
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    bytes.insert(bytes.end(), {0, 0});
    REQUIRE_TRUE(bytes.size() <= std::numeric_limits<std::uint16_t>::max());
    bytes[0] = static_cast<unsigned char>(bytes.size() & 0xFFU);
    bytes[1] = static_cast<unsigned char>(bytes.size() >> 8U);
    bytes.insert(bytes.end(), original.begin() + 37, original.end());
    return bytes;
}

// Purpose: Require full validation before any output or overwrite for a malformed archive.
// Inputs: Exact hostile archive bytes containing the existing nested fixture as a valid prefix.
// Outputs: Throws if extraction succeeds, creates a destination, or alters any existing output.
void require_lha_suffix_rejected(std::span<const unsigned char> bytes) {
    const auto root = test_temp_dir("lha-invalid-suffix");
    const auto archive = root / "invalid.lzh";
    write_lha_fixture(archive, bytes);
    for (const bool overwrite : {false, true}) {
        const auto output = root / (overwrite ? "existing" : "new");
        if (overwrite) {
            std::filesystem::create_directories(output / "subdir" / "subdir2");
            std::ofstream(output / "subdir" / "subdir2" / "hello.txt") << "original";
        }
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::extract_lha(archive, output, overwrite));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        if (overwrite) {
            REQUIRE_EQ(read_text_file(output / "subdir" / "subdir2" / "hello.txt"), "original");
            REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(output),
                                     std::filesystem::recursive_directory_iterator()),
                       3);
        } else {
            REQUIRE_TRUE(!std::filesystem::exists(output));
        }
    }
}

struct LhaMemoryInput {
    std::span<const unsigned char> bytes;
    std::size_t position = 0;
    std::size_t chunk = std::numeric_limits<std::size_t>::max();
    std::size_t fail_at = std::numeric_limits<std::size_t>::max();
    bool fail_skip = false;
    std::size_t skip_calls = 0;
    std::size_t close_calls = 0;
};

// Purpose: Model fragmented successful input and deterministic non-EOF device errors.
// Inputs: A caller-owned memory source, destination, and requested buffer length.
// Outputs: Copies only available bytes, returns zero for physical EOF, or -1 at the injected failure.
int lha_memory_read(void* handle, void* buffer, std::size_t length) {
    auto& input = *static_cast<LhaMemoryInput*>(handle);
    if (input.position >= input.fail_at) {
        return -1;
    }
    const auto size =
        std::min({length, input.chunk, input.bytes.size() - input.position, input.fail_at - input.position});
    std::copy_n(input.bytes.data() + input.position, size, static_cast<unsigned char*>(buffer));
    input.position += size;
    return static_cast<int>(size);
}

// Purpose: Model a checked skip callback, including a deterministic skip failure.
// Inputs: A live memory source and number of bytes to consume.
// Outputs: Advances only across bytes that exist; returns zero on any injected or extent failure.
int lha_memory_skip(void* handle, std::size_t length) {
    auto& input = *static_cast<LhaMemoryInput*>(handle);
    ++input.skip_calls;
    if (input.fail_skip || input.position >= input.fail_at || length > input.bytes.size() - input.position ||
        length > input.fail_at - input.position) {
        return 0;
    }
    input.position += length;
    return 1;
}

// Purpose: Observe release through the callback retained when a stream was created.
// Inputs: The caller-owned memory source, which outlives the stream.
// Outputs: Records one close invocation without freeing caller-owned memory.
void lha_memory_close(void* handle) {
    ++static_cast<LhaMemoryInput*>(handle)->close_calls;
}

struct LhaMemoryReader {
    const LHAInputStreamType type;
    std::unique_ptr<LHAInputStream, decltype(&lha_input_stream_free)> stream;
    std::unique_ptr<LHAReader, decltype(&lha_reader_free)> reader;

    // Purpose: Own a real Lhasa reader over controlled input without replacing parser logic.
    // Inputs: A memory source that outlives the reader and whether to expose the optional checked skip.
    // Outputs: Owns stream/reader resources or fails the test on allocation failure.
    explicit LhaMemoryReader(LhaMemoryInput& input, bool with_skip = false)
        : type{lha_memory_read, with_skip ? lha_memory_skip : nullptr, nullptr},
          stream(lha_input_stream_new(&type, &input), lha_input_stream_free),
          reader(stream ? lha_reader_new(stream.get()) : nullptr, lha_reader_free) {
        REQUIRE_TRUE(stream != nullptr);
        REQUIRE_TRUE(reader != nullptr);
    }
};

}  // namespace

// Purpose: Verify native `.lzh` extraction reads directories and a nested payload through Lhasa.
// Inputs: An embedded LHA archive with `subdir/subdir2/hello.txt`.
// Outputs: Throws if detection, registry state, extraction, or stats regress.
TEST_CASE(lha_extraction_reads_nested_payload) {
    const auto root = test_temp_dir("lha-basic");
    const auto archive = root / "sample.lzh";
    write_lha_fixture(archive, kNestedLhaFixture);

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Lha);
    const auto info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Lha);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_TRUE(!info.gpu_native);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_lha(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("hello world\n").size()));
    REQUIRE_TRUE(!stats.gpu_used);
    REQUIRE_EQ(read_text_file(output / "subdir" / "subdir2" / "hello.txt"), "hello world\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Reject a real late native read failure before publishing an apparently valid archive prefix.
// Inputs: Independent two-member stored archive, unlocked controls and a one-byte second-header read lock.
// Outputs: Both overwrite policies reject without progress or writes; source ownership releases and exact recovery
// succeeds.
TEST_CASE(lha_extraction_rejects_native_read_failure_before_publication) {
    const auto root = test_temp_dir("lha-native-read-failure");
    const auto archive = root / "two-members.lha";
    const std::string first(8160, 'A');
    constexpr std::string_view second = "hello world\n";
    auto bytes = read_failure_lha_member("lead.bin", first);
    const auto tail = read_failure_lha_member("tail.txt", second);
    REQUIRE_EQ(bytes.size(), 8192U);
    REQUIRE_EQ(tail[30], 0x78U);
    REQUIRE_EQ(tail[31], 0x97U);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    bytes.push_back(0);
    REQUIRE_EQ(bytes.size(), 8237U);
    write_lha_fixture(archive, bytes);
    const auto control = root / "control";
    REQUIRE_EQ(flitzzip::extract_lha(archive, control, false).entries, 2U);
    REQUIRE_EQ(read_text_file(control / "lead.bin"), first);
    REQUIRE_EQ(read_text_file(control / "tail.txt"), second);
    for (const bool overwrite : {false, true}) {
        const auto output = root / (overwrite ? "existing" : "new");
        if (overwrite) {
            std::filesystem::create_directories(output);
            std::ofstream(output / "lead.bin", std::ios::binary) << "caller original";
            std::ofstream(output / "sentinel.txt", std::ios::binary) << "unrelated owner";
        }
        std::size_t progress_calls = 0;
        bool rejected = false;
        {
            const LhaFixtureReadLock lock(archive, 8192);
            try {
                static_cast<void>(
                    flitzzip::extract_lha(archive, output, overwrite, [&](const auto&) { ++progress_calls; }));
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(progress_calls, 0U);
        if (overwrite) {
            REQUIRE_EQ(read_text_file(output / "lead.bin"), "caller original");
            REQUIRE_EQ(read_text_file(output / "sentinel.txt"), "unrelated owner");
            REQUIRE_EQ(count_regular_files(output), 2U);
        } else {
            REQUIRE_TRUE(!std::filesystem::exists(output));
        }
        {
            const flitzzip::windows_detail::HandleOwner owner(
                CreateFileW(archive.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            REQUIRE_TRUE(owner && owner.get() != INVALID_HANDLE_VALUE);
        }
        const auto source = read_text_file(archive);
        REQUIRE_EQ(source.size(), bytes.size());
        REQUIRE_TRUE(std::equal(bytes.begin(), bytes.end(), source.begin(), [](unsigned char expected, char actual) {
            return expected == static_cast<unsigned char>(actual);
        }));
        REQUIRE_EQ(flitzzip::extract_lha(archive, output, overwrite).entries, 2U);
        REQUIRE_EQ(read_text_file(output / "lead.bin"), first);
        REQUIRE_EQ(read_text_file(output / "tail.txt"), second);
    }
}

// Purpose: Qualify LHA name/path bounds before CRC work or destination publication with independently framed names.
// Inputs: Valid control, then component/total/directory boundaries with deliberately corrupt payload CRC.
// Outputs: In-bound names reach CRC rejection; excessive names fail earlier, and neither overwrite policy changes
// output.
TEST_CASE(lha_name_metadata_boundary_precedes_crc_and_publication) {
    const auto root = test_temp_dir("lha-name-metadata-boundary");
    const auto archive = root / "input.lzh";
    auto control = named_level_two_lha_member("control.txt", "safe\xFF");
    control.push_back(0);
    write_lha_fixture(archive, control);
    const auto stats = flitzzip::extract_lha(archive, root / "valid", false);
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(read_text_file(root / "valid" / "safe" / "control.txt"), "hello world\n");
    const auto output = root / "rejected";
    std::filesystem::create_directories(output);
    const auto sentinel = output / "existing.txt";
    {
        std::ofstream file(sentinel);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << "retain";
        file.close();
    }
    std::vector<std::pair<std::vector<unsigned char>, std::string_view>> cases;
    for (const auto size : {flitzzip::kMaxArchivePathComponentBytes - 1U, flitzzip::kMaxArchivePathComponentBytes,
                            flitzzip::kMaxArchivePathComponentBytes + 1U}) {
        cases.emplace_back(named_level_two_lha_member(std::string(size, 'a')),
                           size > flitzzip::kMaxArchivePathComponentBytes ? "LHA filename exceeds FlitzZip path limits"
                                                                          : "LHA payload CRC or size check failed");
    }
    constexpr std::string_view leaf = "file.txt";
    for (const auto size :
         {flitzzip::kMaxArchivePathBytes - leaf.size(), flitzzip::kMaxArchivePathBytes - leaf.size() + 1U,
          static_cast<std::size_t>(flitzzip::kMaxArchivePathBytes) + 1U}) {
        std::string directory(size, 'd');
        for (std::size_t offset = 1023U; offset < directory.size(); offset += 1024U) {
            directory[offset] = static_cast<char>(0xFFU);
        }
        directory.back() = static_cast<char>(0xFFU);
        const std::string_view expected =
            size > flitzzip::kMaxArchivePathBytes
                ? "LHA directory path exceeds FlitzZip path limits"
                : (size + leaf.size() > flitzzip::kMaxArchivePathBytes ? "LHA member path exceeds FlitzZip path limits"
                                                                       : "LHA payload CRC or size check failed");
        cases.emplace_back(named_level_two_lha_member(leaf, directory), expected);
    }
    for (auto& [bytes, expected] : cases) {
        bytes.back() ^= 1U;
        bytes.push_back(0);
        write_lha_fixture(archive, bytes);
        for (const bool overwrite : {false, true}) {
            bool rejected = false;
            try {
                (void)flitzzip::extract_lha(archive, output, overwrite);
            } catch (const flitzzip::Error& error) {
                if (std::string_view(error.what()).find(expected) == std::string_view::npos) {
                    throw std::runtime_error("LHA name boundary expected " + std::string(expected) + "; received " +
                                             error.what());
                }
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(read_text_file(sentinel), "retain");
            REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(output),
                                     std::filesystem::recursive_directory_iterator()),
                       1);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify LHA extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: An LHA member path and a preexisting destination file of the same name.
// Outputs: Throws `SecurityError` and preserves the existing file.
TEST_CASE(lha_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("lha-overwrite");
    const auto archive = root / "sample.lzh";
    write_lha_fixture(archive, kNestedLhaFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output / "subdir" / "subdir2");
    std::ofstream(output / "subdir" / "subdir2" / "hello.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "subdir" / "subdir2" / "hello.txt"), "old");
}

// Purpose: Verify LHA extraction rejects truncated archives before output publication.
// Inputs: A fixture truncated to the initial header area.
// Outputs: Throws `ArchiveError` and creates no extracted files.
TEST_CASE(lha_extraction_rejects_truncated_archive_before_output) {
    const auto root = test_temp_dir("lha-truncated");
    const auto archive = root / "truncated.lzh";
    write_lha_fixture(archive, std::span<const unsigned char>(kNestedLhaFixture.data(), 32));

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify LHA extraction rejects corrupted payload bytes through CRC validation.
// Inputs: A valid fixture with one payload byte flipped.
// Outputs: Throws `ArchiveError` before publishing payload files.
TEST_CASE(lha_extraction_rejects_corrupt_payload_crc_before_output) {
    const auto root = test_temp_dir("lha-corrupt-payload");
    const auto archive = root / "corrupt.lzh";
    auto bytes = std::vector<unsigned char>(kNestedLhaFixture.begin(), kNestedLhaFixture.end());
    bytes[196] ^= 0x01U;
    write_lha_fixture(archive, bytes);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify malformed high-bit MS-DOS timestamps do not trigger undefined behavior.
// Inputs: A ClusterFuzzLite UBSan reproducer with a hostile level-0 timestamp field.
// Outputs: Throws a normal FlitzZip error and creates no extracted files.
TEST_CASE(lha_extraction_rejects_ubsan_timestamp_reproducer) {
    const auto root = test_temp_dir("lha-ubsan-timestamp");
    const auto archive = root / "ubsan.lzh";
    write_lha_fixture(archive, kUnsignedTimestampLhaFixture);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify the upstream PM2 copy-table overread reproducer is rejected before publication.
// Inputs: The 45-byte GHSA-j2m3-h278-rrg9 regression archive from the upstream Lhasa fix.
// Outputs: Throws a normal product error, remains sanitizer-clean, and creates no payload file.
TEST_CASE(lha_extraction_rejects_pm2_copy_table_overread_reproducer) {
    const auto root = test_temp_dir("lha-pm2-copy-table-overread");
    const auto archive = root / "evil-pm2.lzh";
    write_lha_fixture(archive, kPm2CopyTableOverreadFixture);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify absolute LHA paths are rejected instead of rewritten.
// Inputs: An upstream regression fixture with `/tmp/...` metadata.
// Outputs: Throws `SecurityError` and does not create escaped output.
TEST_CASE(lha_extraction_rejects_absolute_paths) {
    const auto root = test_temp_dir("lha-absolute-path");
    const auto archive = root / "absolute.lzh";
    write_lha_fixture(archive, kAbsolutePathLhaFixture);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify dot-dot LHA paths remain visible to FlitzZip and are rejected.
// Inputs: An upstream regression fixture with parent-directory path components.
// Outputs: Throws `SecurityError` and does not create escaped output.
TEST_CASE(lha_extraction_rejects_parent_directory_paths) {
    const auto root = test_temp_dir("lha-dotdot");
    const auto archive = root / "dotdot.lzh";
    write_lha_fixture(archive, kDotDotLhaFixture);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    REQUIRE_TRUE(!std::filesystem::exists(root / "evil1.txt"));
    REQUIRE_TRUE(!std::filesystem::exists(root / "evil2.txt"));
}

// Purpose: Verify LHA symbolic-link entries are rejected instead of materialized.
// Inputs: An upstream regression fixture with a Unix symlink entry.
// Outputs: Throws `SecurityError` before creating link targets.
TEST_CASE(lha_extraction_rejects_symbolic_links) {
    const auto root = test_temp_dir("lha-symlink");
    const auto archive = root / "symlink.lzh";
    write_lha_fixture(archive, kSymlinkLhaFixture);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Reject every incomplete next header after a CRC-correct, publishable archive prefix.
// Inputs: Level 0/2/3 headers cut at every byte before their full header extent.
// Outputs: Every non-terminator prefix fails before destination creation or replacement.
TEST_CASE(lha_extraction_rejects_all_partial_trailing_headers_before_publication) {
    for (const unsigned char level : std::array<unsigned char, 3>{0, 2, 3}) {
        const auto member = stored_lha_member(level);
        const auto header_size = member.size() - 12U;
        for (std::size_t cut = 1; cut < header_size; ++cut) {
            std::vector<unsigned char> bytes(kNestedLhaFixture.begin(), kNestedLhaFixture.end() - 1);
            bytes.insert(bytes.end(), member.begin(), member.begin() + static_cast<std::ptrdiff_t>(cut));
            require_lha_suffix_rejected(bytes);
        }
    }
}

// Purpose: Reject complete invalid headers and data after a terminator before publishing valid earlier entries.
// Inputs: Bad checksums, unsupported header levels, short member data, and nonzero terminator suffixes.
// Outputs: Rejects the entire archive and preserves existing destinations for each hostile suffix.
TEST_CASE(lha_extraction_rejects_malformed_trailing_members_and_terminators) {
    auto checksum = stored_lha_member(0);
    checksum[1] ^= 1U;
    auto level = stored_lha_member(0);
    level[20] = 4;
    auto short_payload = stored_lha_member(0);
    short_payload.pop_back();
    auto wide_partial = stored_lha_member(2, true);
    wide_partial.resize(21);
    for (const auto& suffix : {checksum, level, short_payload, wide_partial, std::vector<unsigned char>{0, 1},
                               std::vector<unsigned char>(4097, 0)}) {
        std::vector<unsigned char> bytes(kNestedLhaFixture.begin(), kNestedLhaFixture.end() - 1);
        bytes.insert(bytes.end(), suffix.begin(), suffix.end());
        if (suffix.size() == 4097) {
            bytes.push_back(1);  // Invalid data beyond the zero-tail reader's first buffer.
        }
        require_lha_suffix_rejected(bytes);
    }
}

// Purpose: Reject truncated unread directory data instead of seeking beyond physical EOF.
// Inputs: A valid archive prefix and a level-1 directory declaring 32 compressed bytes.
// Outputs: Every incomplete extent preserves destinations; the complete extent extracts correctly.
TEST_CASE(lha_extraction_checks_unread_directory_extent) {
    constexpr std::size_t data_size = 32;
    constexpr std::size_t header_size = kNestedLhaFixture[0] + 2U + kNestedLhaFixture[7];
    std::vector<unsigned char> directory(kNestedLhaFixture.begin(), kNestedLhaFixture.begin() + header_size);
    directory[1] = static_cast<unsigned char>(directory[1] + data_size);
    directory[7] = static_cast<unsigned char>(directory[7] + data_size);
    constexpr std::array<unsigned char, 6> name{'b', 'r', 'o', 'k', 'e', 'n'};
    std::copy(name.begin(), name.end(), directory.begin() + 40);
    std::vector<unsigned char> bytes(kNestedLhaFixture.begin(), kNestedLhaFixture.end() - 1);
    bytes.insert(bytes.end(), directory.begin(), directory.end());
    for (std::size_t present = 0; present < data_size; ++present) {
        require_lha_suffix_rejected(bytes);
        bytes.push_back(0);
    }
    const auto root = test_temp_dir("lha-directory-extent");
    const auto archive = root / "complete.lzh";
    write_lha_fixture(archive, bytes);
    static_cast<void>(flitzzip::extract_lha(archive, root / "out", false));
    REQUIRE_TRUE(std::filesystem::is_directory(root / "out" / "broken"));
    REQUIRE_EQ(read_text_file(root / "out" / "subdir" / "subdir2" / "hello.txt"), "hello world\n");
}

// Purpose: Preserve valid header levels and documented clean termination forms.
// Inputs: Independent level 0/2/3 members, including a zero-low-byte level-2 length, with EOF/zero padding.
// Outputs: Exact payload readback succeeds for each encoding and termination.
TEST_CASE(lha_extraction_accepts_header_levels_and_clean_termination) {
    for (const unsigned char level : std::array<unsigned char, 3>{0, 2, 3}) {
        for (const std::size_t padding : {0U, 1U, 2U, 22U, 4097U}) {
            for (const bool wide : {false, true}) {
                if (wide && level != 2) {
                    continue;
                }
                const auto root = test_temp_dir("lha-clean-end");
                const auto archive = root / "valid.lzh";
                auto bytes = stored_lha_member(level, wide);
                bytes.insert(bytes.end(), padding, 0);
                write_lha_fixture(archive, bytes);
                const auto stats = flitzzip::extract_lha(archive, root / "out", false);
                REQUIRE_EQ(stats.entries, 1U);
                REQUIRE_EQ(stats.output_bytes, 12U);
                REQUIRE_EQ(read_text_file(root / "out" / "file.txt"), "hello world\n");
            }
        }
    }
}

// Purpose: Preserve PMarc's CP/M record padding without accepting arbitrary or unbounded suffixes.
// Inputs: A stored PMarc member with exact CRC and valid, mixed, or excessive padding.
// Outputs: Valid records extract exactly; invalid suffixes fail before any publication or overwrite.
TEST_CASE(lha_extraction_validates_pmarc_record_padding) {
    auto member = stored_lha_member(0);
    member[3] = 'p';
    member[4] = 'm';
    member[1] = static_cast<unsigned char>(member[1] + ('p' - 'l') + ('m' - 'h'));
    for (const std::size_t padding : {1U, 39U, 95U, 127U, 128U}) {
        std::vector<unsigned char> bytes(kNestedLhaFixture.begin(), kNestedLhaFixture.end() - 1);
        bytes.insert(bytes.end(), member.begin(), member.end());
        bytes.push_back(0);
        bytes.insert(bytes.end(), padding, 0x1a);
        if (padding == 128) {
            require_lha_suffix_rejected(bytes);
        } else {
            const auto root = test_temp_dir("lha-pmarc-end");
            const auto archive = root / "valid.lzh";
            write_lha_fixture(archive, bytes);
            const auto stats = flitzzip::extract_lha(archive, root / "out", false);
            REQUIRE_EQ(stats.entries, 4U);
            REQUIRE_EQ(read_text_file(root / "out" / "file.txt"), "hello world\n");
            REQUIRE_EQ(read_text_file(root / "out" / "subdir" / "subdir2" / "hello.txt"), "hello world\n");
        }
        bytes.back() = padding > 1 ? 0 : 2;  // Mixed or invalid padding must not disguise a new header.
        require_lha_suffix_rejected(bytes);
    }
    member = stored_lha_member(0);
    std::vector<unsigned char> bytes(kNestedLhaFixture.begin(), kNestedLhaFixture.end() - 1);
    bytes.insert(bytes.end(), member.begin(), member.end());
    bytes.insert(bytes.end(), {0, 0x1a});
    require_lha_suffix_rejected(bytes);  // CP/M padding is specific to PMarc.
}

// Purpose: Distinguish real EOF from callback failures, even after a valid last member or terminator.
// Inputs: An actual Lhasa reader with a failure at each byte position, plus successful fragmented controls.
// Outputs: Every device error is sticky ERROR; clean readers reach sticky END with a cleared header.
TEST_CASE(lha_reader_preserves_input_errors_and_fragmented_reads) {
    auto bytes = stored_lha_member(0);
    bytes.push_back(0);
    for (std::size_t fail_at = 0; fail_at <= bytes.size(); ++fail_at) {
        LhaMemoryInput input{.bytes = bytes, .chunk = 3, .fail_at = fail_at};
        LhaMemoryReader session(input);
        LHAFileHeader* header = nullptr;
        auto result = lha_reader_next_file(session.reader.get(), &header);
        if (result == LHA_READ_OK) {
            static_cast<void>(lha_reader_check(session.reader.get(), nullptr, nullptr));
            result = lha_reader_next_file(session.reader.get(), &header);
        }
        REQUIRE_EQ(result, LHA_READ_ERROR);
        REQUIRE_TRUE(header == nullptr);
        REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), LHA_READ_ERROR);
        REQUIRE_TRUE(header == nullptr);
    }
    for (const std::size_t chunk : {1U, 3U, 24U, 256U}) {
        LhaMemoryInput input{.bytes = bytes, .chunk = chunk};
        LhaMemoryReader session(input);
        LHAFileHeader* header = nullptr;
        REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), LHA_READ_OK);
        REQUIRE_TRUE(header != nullptr);
        REQUIRE_EQ(lha_reader_check(session.reader.get(), nullptr, nullptr), 1);
        REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), LHA_READ_END);
        REQUIRE_TRUE(header == nullptr);
        REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), LHA_READ_END);
    }
}

// Purpose: Reject failed compressed-data skips instead of completing a shortened archive.
// Inputs: Unread payloads, checked skip failures and short physical data without a skip callback.
// Outputs: Failed skips remain ERROR; complete unread payloads are consumed before a clean END.
TEST_CASE(lha_reader_preserves_compressed_skip_failures) {
    const auto full = stored_lha_member(0);
    for (const bool with_skip : {false, true}) {
        for (const bool truncated : {false, true}) {
            auto bytes = full;
            if (truncated) {
                bytes.resize(bytes.size() - 5);
            }
            LhaMemoryInput input{.bytes = bytes, .fail_skip = with_skip};
            LhaMemoryReader session(input, with_skip);
            LHAFileHeader* header = nullptr;
            REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), LHA_READ_OK);
            const auto expected = with_skip || truncated ? LHA_READ_ERROR : LHA_READ_END;
            REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), expected);
            REQUIRE_TRUE(header == nullptr);
            REQUIRE_EQ(lha_reader_next_file(session.reader.get(), &header), expected);
            REQUIRE_EQ(input.skip_calls, with_skip ? 1U : 0U);
        }
    }
}

// Purpose: Verify buffered lead-in bytes and callback bytes share one logical skip position.
// Inputs: A recognized member read one byte at a time, with and without a dedicated skip callback.
// Outputs: Reads the byte at the exact logical offset after skipping across the buffered region.
TEST_CASE(lha_stream_skip_consumes_buffered_bytes_before_callback) {
    const auto bytes = stored_lha_member(0);
    for (const bool with_skip : {false, true}) {
        LhaMemoryInput input{.bytes = bytes};
        LhaMemoryReader session(input, with_skip);
        unsigned char byte = 0;
        REQUIRE_EQ(lha_input_stream_read(session.stream.get(), &byte, 1), 1);
        REQUIRE_EQ(byte, bytes[0]);
        REQUIRE_EQ(lha_input_stream_skip(session.stream.get(), 30), 1);
        REQUIRE_EQ(lha_input_stream_read(session.stream.get(), &byte, 1), 1);
        REQUIRE_EQ(byte, bytes[31]);
        REQUIRE_EQ(input.skip_calls, with_skip ? 1U : 0U);
    }
}

// Purpose: Keep active stream behavior independent from a caller's callback table lifetime.
// Inputs: A valid memory stream whose original skip/close slots are changed after construction.
// Outputs: The stream still uses its original callbacks, including exactly one close.
TEST_CASE(lha_stream_owns_callback_configuration) {
    const auto bytes = stored_lha_member(0);
    LhaMemoryInput input{bytes};
    LHAInputStreamType callbacks{lha_memory_read, lha_memory_skip, lha_memory_close};
    std::unique_ptr<LHAInputStream, decltype(&lha_input_stream_free)> stream(lha_input_stream_new(&callbacks, &input),
                                                                             lha_input_stream_free);
    REQUIRE_TRUE(stream != nullptr);
    callbacks.skip = nullptr;
    callbacks.close = nullptr;
    std::array<unsigned char, 32> header{};
    REQUIRE_TRUE(lha_input_stream_read(stream.get(), header.data(), header.size()) != 0);
    REQUIRE_TRUE(lha_input_stream_skip(stream.get(), 12) != 0);
    REQUIRE_EQ(input.skip_calls, 1U);
    REQUIRE_EQ(input.position, bytes.size());
    REQUIRE_EQ(input.close_calls, 0U);
    stream.reset();
    REQUIRE_EQ(input.close_calls, 1U);
}
