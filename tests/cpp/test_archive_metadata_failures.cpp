#include "test_util.hpp"

#include "core/archive.hpp"
#include "core/archive_blocks.hpp"
#include "core/archive_index.hpp"
#include "core/result.hpp"
#include "test_flzip_helpers.hpp"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_codec_support.hpp"
#endif

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <streambuf>
#include <utility>

namespace {

using Path = std::filesystem::path;
using Kind = flitzzip::BlockKind;

// Independently packed little-endian index: one raw "bin" entry containing "foo".
constexpr std::array<unsigned char, 68> kIndexWire{83, 85, 90, 80, 1, 0, 0, 0, 1,  0,   0,   0,   3, 0, 98, 105, 110,
                                                   0,  3,  0,  0,  0, 0, 0, 0, 0,  0,   0,   0,   0, 0, 0,  0,   0,
                                                   3,  0,  0,  0,  0, 0, 0, 0, 33, 101, 115, 140, 1, 0, 0,  0,   0,
                                                   0,  3,  0,  0,  0, 0, 0, 0, 0,  0,   0,   0,   0, 3, 0,  0,   0};

// Purpose: Model a bounded, forward-only metadata source without weakening production stream checks.
// Inputs: Owned wire bytes. Outputs: Standard reads through EOF; inherited seek operations remain unsupported.
class ForwardOnlyIndexBuffer final : public std::streambuf {
    std::string bytes_;

  public:
    // Purpose: Own the complete test input. Inputs: Bounded wire bytes. Outputs: A stable read area until destruction.
    explicit ForwardOnlyIndexBuffer(std::string bytes) : bytes_(std::move(bytes)) {
        setg(bytes_.data(), bytes_.data(), bytes_.data() + bytes_.size());
    }
};

enum class IndexSeekBehavior { TellOnly, FailedEndAfterMove, RefusedRestore };

// Purpose: Model independent stream capabilities and partial failures in optional metadata-size probes.
// Inputs: Owned fixture bytes and one seek behavior. Outputs: Ordinary reads plus the selected seek contract.
class PartialSeekIndexBuffer final : public std::stringbuf {
    IndexSeekBehavior behavior_;

    // Purpose: Allow position queries while independently controlling end-seek support.
    // Inputs: Standard stream buffer seek arguments. Outputs: Position or failure, optionally after moving to end.
    pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode mode) override {
        if (direction != std::ios::end) {
            return std::stringbuf::seekoff(offset, direction, mode);
        }
        if (behavior_ == IndexSeekBehavior::TellOnly) {
            return pos_type(off_type(-1));
        }
        const auto position = std::stringbuf::seekoff(offset, direction, mode);
        return behavior_ == IndexSeekBehavior::FailedEndAfterMove ? pos_type(off_type(-1)) : position;
    }

    // Purpose: Distinguish a recoverable failed probe from an input that cannot restore its previous position.
    // Inputs: Requested position and open mode. Outputs: Restored position or explicit unsupported-operation result.
    pos_type seekpos(pos_type position, std::ios::openmode mode) override {
        return behavior_ == IndexSeekBehavior::FailedEndAfterMove ? std::stringbuf::seekpos(position, mode)
                                                                  : pos_type(off_type(-1));
    }

  public:
    // Purpose: Own a bounded input with explicit seek capabilities. Inputs: Bytes and behavior. Outputs: Readable
    // buffer.
    PartialSeekIndexBuffer(const std::string& bytes, IndexSeekBehavior behavior)
        : std::stringbuf(bytes, std::ios::in), behavior_(behavior) {}
};

// Purpose: Model an output that fails exactly at a selected byte without filling a disk or changing host state.
// Inputs: Byte limit. Outputs: Retained successful prefix; subsequent writes return EOF.
class LimitedIndexOutput final : public std::streambuf {
    std::size_t limit_;
    std::string bytes_;

    // Purpose: Admit one output byte within the limit. Inputs: Character or EOF. Outputs: Written character or EOF.
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        if (bytes_.size() == limit_) {
            return traits_type::eof();
        }
        bytes_.push_back(traits_type::to_char_type(value));
        return value;
    }

  public:
    // Purpose: Set a deterministic output bound. Inputs: Maximum accepted bytes. Outputs: Empty owned output.
    explicit LimitedIndexOutput(std::size_t limit) : limit_(limit) {}
    // Purpose: Inspect accepted bytes. Inputs: None. Outputs: Borrowed view valid until the next write/destruction.
    std::string_view bytes() const {
        return bytes_;
    }
};

// Purpose: Select the version in an independently packed raw index without invoking the production writer.
// Inputs: One readable version. Outputs: Exact small wire fixture with only the version byte changed.
std::string index_wire_fixture(std::uint32_t version) {
    REQUIRE_TRUE(version >= 1U && version <= 11U);
    std::string bytes(kIndexWire.begin(), kIndexWire.end());
    bytes[4] = static_cast<char>(version);
    return bytes;
}

// Purpose: Check every field against the independent raw-index fixture.
// Inputs: Parsed index and expected version. Outputs: Assertions fail on lost or incorrectly decoded metadata.
void require_wire_index(const flitzzip::ArchiveIndex& index, std::uint32_t version) {
    REQUIRE_EQ(index.version, version);
    REQUIRE_EQ(index.entries.size(), 1U);
    const auto& entry = index.entries.front();
    REQUIRE_EQ(entry.path, "bin");
    REQUIRE_TRUE(!entry.directory);
    REQUIRE_EQ(entry.uncompressed_size, 3U);
    REQUIRE_EQ(entry.payload_offset, 0U);
    REQUIRE_EQ(entry.payload_size, 3U);
    REQUIRE_EQ(entry.crc32, 0x8C736521U);
    REQUIRE_EQ(entry.blocks.size(), 1U);
    const auto& block = entry.blocks.front();
    REQUIRE_EQ(block.kind, Kind::Raw);
    REQUIRE_EQ(block.fill_value, 0U);
    REQUIRE_EQ(block.uncompressed_len, 3U);
    REQUIRE_EQ(block.encoded_offset, 0U);
    REQUIRE_EQ(block.encoded_len, 3U);
}

// Purpose: Prove optional seeking cannot hide truncated fields or consume the caller's following record.
// Inputs: Independent index bytes, all readable versions and every shorter prefix on a forward-only source.
// Outputs: Exact valid metadata and untouched sentinel; every truncated prefix throws ArchiveError.
TEST_CASE(flzip_index_forward_only_reads_preserve_framing_and_reject_every_truncation) {
    for (std::uint32_t version = 1U; version <= flitzzip::kFlitzZipMaxReadableVersion; ++version) {
        const auto bytes = index_wire_fixture(version);
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            ForwardOnlyIndexBuffer buffer(bytes.substr(0, length));
            std::istream input(&buffer);
            bool rejected = false;
            try {
                static_cast<void>(flitzzip::read_archive_index(input));
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
        ForwardOnlyIndexBuffer buffer(bytes + '\x7F');
        std::istream input(&buffer);
        require_wire_index(flitzzip::read_archive_index(input), version);
        REQUIRE_TRUE(input.good());
        REQUIRE_EQ(input.get(), 127);
        REQUIRE_EQ(input.get(), std::char_traits<char>::eof());
    }
}

// Purpose: Prevent optional stream-size probing from changing parsing results or caller exception policy.
// Inputs: Valid independent bytes, partially seekable inputs, every readable version and both exception modes.
// Outputs: Exact metadata and sentinel readback after unsupported or position-changing failed end seeks.
TEST_CASE(flzip_index_optional_seeking_preserves_input_position_and_exception_mask) {
    for (std::uint32_t version = 1U; version <= flitzzip::kFlitzZipMaxReadableVersion; ++version) {
        for (const auto behavior : {IndexSeekBehavior::TellOnly, IndexSeekBehavior::FailedEndAfterMove}) {
            for (const bool exceptions : {false, true}) {
                PartialSeekIndexBuffer buffer(index_wire_fixture(version) + '\x7F', behavior);
                std::istream input(&buffer);
                const auto mask = exceptions ? std::ios::badbit | std::ios::failbit : std::ios::goodbit;
                input.exceptions(mask);
                require_wire_index(flitzzip::read_archive_index(input), version);
                REQUIRE_EQ(input.exceptions(), mask);
                REQUIRE_TRUE(input.good());
                REQUIRE_EQ(input.get(), 127);
            }
        }
    }
}

// Purpose: Reject an irreversible size-probe seek before reading records from the wrong input position.
// Inputs: A stream that can seek to end but refuses restoration and both exception policies.
// Outputs: A specific restoration failure; no successful parse and no mutation of the caller's exception mask.
TEST_CASE(flzip_index_failed_position_restore_rejects_before_record_reads) {
    for (const bool exceptions : {false, true}) {
        PartialSeekIndexBuffer buffer(index_wire_fixture(1U), IndexSeekBehavior::RefusedRestore);
        std::istream input(&buffer);
        const auto mask = exceptions ? std::ios::badbit | std::ios::failbit : std::ios::goodbit;
        input.exceptions(mask);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::read_archive_index(input));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string_view(error.what()), "failed to restore archive metadata stream position");
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(input.exceptions(), mask);
    }
}

// Purpose: Detect short output at every encoded position, including metadata paths and descriptor tails.
// Inputs: Independently decoded control, all readable versions, bounded outputs and both stream exception modes.
// Outputs: Failed writes cannot report success; complete writes exactly match the independent wire bytes.
TEST_CASE(flzip_index_writer_propagates_every_short_write) {
    for (std::uint32_t version = 1U; version <= flitzzip::kFlitzZipMaxReadableVersion; ++version) {
        const auto bytes = index_wire_fixture(version);
        std::istringstream input(bytes, std::ios::binary);
        const auto index = flitzzip::read_archive_index(input);
        require_wire_index(index, version);
        for (const bool exceptions : {false, true}) {
            for (std::size_t limit = 0; limit <= bytes.size(); ++limit) {
                LimitedIndexOutput buffer(limit);
                std::ostream output(&buffer);
                if (exceptions) {
                    output.exceptions(std::ios::badbit | std::ios::failbit);
                }
                bool rejected = false;
                try {
                    flitzzip::write_archive_index(output, index);
                } catch (const flitzzip::ArchiveError&) {
                    rejected = true;
                } catch (const std::ios_base::failure&) {
                    REQUIRE_TRUE(exceptions);
                    rejected = true;
                }
                REQUIRE_EQ(rejected, limit < bytes.size());
                REQUIRE_EQ(buffer.bytes(), std::string_view(bytes).substr(0, limit));
                REQUIRE_EQ(output.good(), limit == bytes.size());
            }
        }
    }
}

// Purpose: Reject hostile header identities and allocation counts even when stream size cannot be queried.
// Inputs: Independently encoded control with one invalid little-endian header/count field per case.
// Outputs: Specific ArchiveError diagnostics, followed by successful reuse of the unmodified fixture.
TEST_CASE(flzip_index_forward_only_rejects_invalid_headers_and_resource_counts) {
    struct InvalidField {
        std::size_t offset;
        std::uint32_t value;
        const char* diagnostic;
    };
    const std::array cases{
        InvalidField{0U, 0U, "not a FlitzZip archive"},
        InvalidField{4U, 0U, "unsupported FlitzZip archive version"},
        InvalidField{4U, 12U, "unsupported FlitzZip archive version"},
        InvalidField{4U, std::numeric_limits<std::uint32_t>::max(), "unsupported FlitzZip archive version"},
        InvalidField{8U, flitzzip::kMaxArchiveEntries + 1U, "archive entry count is unreasonable"},
        InvalidField{46U, flitzzip::kMaxBlocksPerEntry + 1U, "archive block count is unreasonable"},
    };
    for (const auto& invalid : cases) {
        auto bytes = index_wire_fixture(1U);
        for (std::size_t i = 0; i < 4U; ++i) {
            bytes[invalid.offset + i] = static_cast<char>((invalid.value >> (8U * i)) & 255U);
        }
        ForwardOnlyIndexBuffer buffer(std::move(bytes));
        std::istream input(&buffer);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::read_archive_index(input));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string_view(error.what()), invalid.diagnostic);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    ForwardOnlyIndexBuffer buffer(index_wire_fixture(1U));
    std::istream input(&buffer);
    require_wire_index(flitzzip::read_archive_index(input), 1U);
}

// Purpose: Keep unsupported format versions from modifying a caller's output stream.
// Inputs: Caller-owned prefix and empty indexes with invalid versions. Outputs: Exact prefix preservation on error.
TEST_CASE(flzip_index_writer_rejects_versions_before_output) {
    for (const auto version : std::array{0U, 12U, std::numeric_limits<std::uint32_t>::max()}) {
        flitzzip::ArchiveIndex index;
        index.version = version;
        std::ostringstream output(std::ios::binary);
        output << "preserve caller prefix";
        bool rejected = false;
        try {
            flitzzip::write_archive_index(output, index);
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string_view(error.what()), "unsupported FlitzZip archive version");
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(output.str(), "preserve caller prefix");
    }
}

// Independent Python zlib output for 1024 zero bytes, with CRC-32 4021661486.
constexpr std::array<unsigned char, 17> kDeflateZeroFrame{120, 156, 99, 96, 24, 5, 163, 96, 20,
                                                          140, 84,  0,  0,  4,  0, 0,   1};

// Purpose: Emit small hostile wire records without asking the production writer to admit invalid descriptors.
// Inputs: One entry, actual bounded payload and declared version. Outputs: A closed archive with independent framing.
void write_metadata_fixture(const Path& path, const flitzzip::ArchiveEntry& entry, std::string_view payload = "foo",
                            std::uint32_t version = 11) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto offset = static_cast<std::uint64_t>(output.tellp());
    flitzzip::write_u32(output, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(output, version);
    flitzzip::write_u32(output, 1);
    flitzzip::write_u16(output, static_cast<std::uint16_t>(entry.path.size()));
    output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
    output.put(entry.directory ? '\1' : '\0');
    flitzzip::write_u64(output, entry.uncompressed_size);
    flitzzip::write_u64(output, entry.payload_offset);
    flitzzip::write_u64(output, entry.payload_size);
    flitzzip::write_u32(output, entry.crc32);
    flitzzip::write_u32(output, static_cast<std::uint32_t>(entry.blocks.size()));
    for (const auto& block : entry.blocks) {
        output.put(static_cast<char>(block.kind));
        output.put(static_cast<char>(block.fill_value));
        flitzzip::write_u32(output, block.uncompressed_len);
        flitzzip::write_u64(output, block.encoded_offset);
        flitzzip::write_u32(output, block.encoded_len);
    }
    const auto index_bytes = static_cast<std::uint64_t>(output.tellp()) - offset;
    flitzzip_test::write_test_footer(output, offset, index_bytes, version);
    output.close();
}

// Purpose: Provide a positive native wire control independent of FlitzZip compression.
// Inputs: None. Outputs: One raw foo entry with an independently known CRC-32.
flitzzip::ArchiveEntry raw_control() {
    return {.path = "payload.bin",
            .uncompressed_size = 3,
            .payload_size = 3,
            .crc32 = 0x8C736521U,
            .blocks = {{.kind = Kind::Raw, .uncompressed_len = 3, .encoded_len = 3}}};
}

// Purpose: Require a particular validation boundary to reject before output publication.
// Inputs: Small fixture and expected diagnostic fragment. Outputs: Both verify and extract reject, preserving bytes.
void require_metadata_rejection(const Path& archive, const Path& destination, std::string_view diagnostic) {
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (const bool verify : {true, false}) {
        bool rejected = false;
        try {
            if (verify) {
                (void)flitzzip::verify_flzip(archive, options);
            } else {
                (void)flitzzip::extract_flzip(archive, destination, options);
            }
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        std::ifstream original(destination / "payload.bin", std::ios::binary);
        REQUIRE_TRUE(original.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(original), {}), "preserve destination");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 1);
    }
}

// Purpose: Own bounded hostile metadata fixtures and the destination that must survive them.
// Inputs: Test label. Outputs: Isolated files and no leftover directory on scope exit.
struct MetadataFixture {
    Path root;
    Path archive;
    Path destination;
    // Purpose: Establish a sentinel target. Inputs: Stable label. Outputs: An existing destination with known bytes.
    explicit MetadataFixture(const char* label)
        : root(test_temp_dir(label)), archive(root / "test.flzip"), destination(root / "out") {
        std::filesystem::create_directory(destination);
        std::ofstream output(destination / "payload.bin", std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << "preserve destination";
        output.close();
    }
    // Purpose: Remove closed owned fixtures. Inputs: Owned root. Outputs: Best-effort cleanup.
    ~MetadataFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// Purpose: Write independently modified index bytes while retaining a bounded payload and explicit footer.
// Inputs: Owned fixture path, complete prefix bytes and declared index offset/size/version.
// Outputs: Closes a small test archive without validating or normalizing the hostile records.
void write_index_framing_fixture(const Path& path, std::string_view prefix, std::uint64_t index_offset,
                                 std::uint64_t index_size, std::uint32_t version) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    flitzzip_test::write_test_footer(output, index_offset, index_size, version);
    output.close();
}

// Purpose: Reject ambiguous index extents before decoding or publishing any destination.
// Inputs: Every readable version with independently changed index size, gaps, footer overlap and trailing data.
// Outputs: Malformed framing is rejected, existing bytes survive and a restored valid archive verifies.
TEST_CASE(flzip_index_framing_rejects_unconsumed_and_overlapping_bytes) {
    using namespace flitzzip;
    MetadataFixture fixture("index-framing");
    const ExtractOptions options{.gpu_required = false, .force_cpu = true};
    for (std::uint32_t version = 1; version <= kFlitzZipMaxReadableVersion; ++version) {
        write_metadata_fixture(fixture.archive, raw_control(), "foo", version);
        std::string prefix;
        {
            std::ifstream input(fixture.archive, std::ios::binary);
            prefix.assign(std::istreambuf_iterator<char>(input), {});
        }
        prefix.resize(prefix.size() - kFlitzZipFooterBytes);
        const auto index_size = prefix.size() - 3U;
        for (const auto extra : std::array<std::uint64_t, 2>{1U, kFlitzZipFooterBytes}) {
            write_index_framing_fixture(fixture.archive, prefix, 3U, index_size + extra, version);
            require_metadata_rejection(fixture.archive, fixture.destination, "index does not end at footer");
        }
        write_index_framing_fixture(fixture.archive, prefix, 3U, index_size - 1U, version);
        require_metadata_rejection(fixture.archive, fixture.destination, "index does not end at footer");
        write_index_framing_fixture(fixture.archive, prefix + '\0', 3U, index_size, version);
        require_metadata_rejection(fixture.archive, fixture.destination, "index does not end at footer");
        write_index_framing_fixture(fixture.archive, prefix + '\0', 3U, index_size + 1U, version);
        require_metadata_rejection(fixture.archive, fixture.destination, "index contains trailing bytes");
        write_index_framing_fixture(fixture.archive, prefix, prefix.size() + 1U, 0U, version);
        require_metadata_rejection(fixture.archive, fixture.destination, "index does not end at footer");
        write_metadata_fixture(fixture.archive, raw_control(), "foo", version);
        REQUIRE_EQ(verify_flzip(fixture.archive, options).output_bytes, 3U);
    }
}

// Purpose: Prevent entries from interpreting index/footer bytes as their compressed or raw payload.
// Inputs: Valid raw metadata with payload windows crossing or starting beyond the payload region.
// Outputs: Verification and extraction reject before destination writes, including overflowing offsets.
TEST_CASE(flzip_index_framing_rejects_payload_metadata_aliases) {
    using namespace flitzzip;
    MetadataFixture fixture("payload-metadata-alias");
    for (std::uint32_t version = 1; version <= kFlitzZipMaxReadableVersion; ++version) {
        auto alias = raw_control();
        alias.uncompressed_size = 4;
        alias.payload_size = 4;
        alias.crc32 = 0xB9B47721U;  // Independently computed CRC-32 of the index magic SUZP.
        alias.blocks.front().uncompressed_len = 4;
        alias.blocks.front().encoded_len = 4;
        write_metadata_fixture(fixture.archive, alias, "", version);
        require_metadata_rejection(fixture.archive, fixture.destination, "payload overlaps index or footer");
        for (const auto offset : {1ULL, 3ULL, 4ULL, std::numeric_limits<std::uint64_t>::max()}) {
            auto entry = raw_control();
            entry.payload_offset = offset;
            write_metadata_fixture(fixture.archive, entry, "foo", version);
            require_metadata_rejection(fixture.archive, fixture.destination, "payload overlaps index or footer");
        }
        auto empty = raw_control();
        empty.uncompressed_size = 0;
        empty.payload_size = 0;
        empty.crc32 = 0;
        empty.blocks.clear();
        for (const bool directory : {false, true}) {
            empty.directory = directory;
            empty.payload_offset = 4;
            write_metadata_fixture(fixture.archive, empty, "foo", version);
            require_metadata_rejection(fixture.archive, fixture.destination, "payload overlaps index or footer");
            empty.payload_offset = 3;
            write_metadata_fixture(fixture.archive, empty, "foo", version);
            REQUIRE_EQ(verify_flzip(fixture.archive, {.gpu_required = false, .force_cpu = true}).entries, 1U);
        }
    }
}

// Purpose: Keep the wire directory flag boolean instead of accepting unknown values as directories.
// Inputs: Independent empty-entry bytes covering every flag value and readable version.
// Outputs: Only zero and one parse; hostile flags also fail public verify/extract without destination changes.
TEST_CASE(flzip_index_framing_requires_boolean_directory_flags) {
    using namespace flitzzip;
    MetadataFixture fixture("directory-flag");
    for (std::uint32_t version = 1; version <= kFlitzZipMaxReadableVersion; ++version) {
        std::ostringstream wire(std::ios::binary);
        write_u32(wire, kFlitzZipMagic);
        write_u32(wire, version);
        write_u32(wire, 1U);
        write_u16(wire, 11U);
        wire.write("payload.bin", 11);
        wire.put('\0');
        write_u64(wire, 0U);
        write_u64(wire, 0U);
        write_u64(wire, 0U);
        write_u32(wire, 0U);
        write_u32(wire, 0U);
        auto index_bytes = wire.str();
        for (unsigned flag = 0; flag <= 255; ++flag) {
            index_bytes[25] = static_cast<char>(flag);
            std::istringstream input(index_bytes, std::ios::binary);
            bool rejected = false;
            try {
                const auto index = read_archive_index(input);
                REQUIRE_EQ(index.entries.size(), 1U);
                REQUIRE_EQ(index.entries.front().directory, flag == 1U);
            } catch (const ArchiveError& error) {
                REQUIRE_TRUE(std::string_view(error.what()).find("directory flag is invalid") !=
                             std::string_view::npos);
                rejected = true;
            }
            REQUIRE_EQ(rejected, flag > 1U);
            if (flag == 2U || flag == 255U) {
                write_index_framing_fixture(fixture.archive, index_bytes, 0U, index_bytes.size(), version);
                require_metadata_rejection(fixture.archive, fixture.destination, "directory flag is invalid");
            }
        }
    }
}

}  // namespace

// Purpose: Validate a positive raw fixture through every readable native format version.
// Inputs: Independently framed foo bytes and its CRC-32, with versions one through eleven.
// Outputs: Verify and extraction both agree on bytes/counts without requiring GPU hardware.
TEST_CASE(flzip_metadata_positive_controls_cover_readable_versions) {
    MetadataFixture fixture("native-metadata-positive");
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (std::uint32_t version = 1; version <= 11; ++version) {
        write_metadata_fixture(fixture.archive, raw_control(), "foo", version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);
        REQUIRE_EQ(flitzzip::extract_flzip(fixture.archive, fixture.destination, options).output_bytes, 3U);
        std::ifstream restored(fixture.destination / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), "foo");
    }
}

// Purpose: Reject incomplete or overlong Deflate streams without replacing existing destination bytes.
// Inputs: An independent zlib frame, every byte truncation, suffixes, and wrong decoded lengths in all readable
// versions. Outputs: Both public readers reject each malformed frame and subsequently verify/extract the intact frame
// exactly.
TEST_CASE(flzip_deflate_frames_require_exact_consumption) {
    const std::string frame(kDeflateZeroFrame.begin(), kDeflateZeroFrame.end());
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (std::uint32_t version = 1; version <= 11; ++version) {
        MetadataFixture fixture("native-deflate-frame");
        auto entry = raw_control();
        entry.uncompressed_size = 1024;
        entry.payload_size = frame.size();
        entry.crc32 = 4021661486U;
        entry.blocks[0] = {
            .kind = Kind::Deflate, .uncompressed_len = 1024, .encoded_len = static_cast<std::uint32_t>(frame.size())};
        write_metadata_fixture(fixture.archive, entry, frame, version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);

        std::vector<std::string> malformed{frame + '\0', frame + frame};
        for (std::size_t length = 1; length < frame.size(); ++length) {
            malformed.push_back(frame.substr(0, length));
        }
        for (const auto& payload : malformed) {
            auto invalid = entry;
            invalid.payload_size = payload.size();
            invalid.blocks[0].encoded_len = static_cast<std::uint32_t>(payload.size());
            write_metadata_fixture(fixture.archive, invalid, payload, version);
            require_metadata_rejection(fixture.archive, fixture.destination, "deflate block failed to decompress");
        }
        for (const auto length : {1023U, 1025U}) {
            auto invalid = entry;
            invalid.uncompressed_size = length;
            invalid.blocks[0].uncompressed_len = length;
            write_metadata_fixture(fixture.archive, invalid, frame, version);
            require_metadata_rejection(fixture.archive, fixture.destination, "deflate block failed to decompress");
        }
        write_metadata_fixture(fixture.archive, entry, frame, version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);
        REQUIRE_EQ(flitzzip::extract_flzip(fixture.archive, fixture.destination, options).output_bytes, 1024U);
        std::ifstream restored(fixture.destination / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), std::string(1024, '\0'));
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(fixture.destination),
                                 std::filesystem::directory_iterator{}),
                   1);
    }
}

// Purpose: Enforce Deflate input consumption in both whole-chunk and mixed CPU/GPU decode orchestration.
// Inputs: A frame between two raw blocks, an overlong declared frame, and serial/parallel worker budgets.
// Outputs: Consumers reject excess bytes, recover, and modify only the block ranges each consumer owns.
TEST_CASE(flzip_deflate_consumers_preserve_block_boundaries_and_recover) {
    std::vector<std::byte> payload{std::byte{0x11}};
    const auto frame = std::as_bytes(std::span(kDeflateZeroFrame));
    payload.insert(payload.end(), frame.begin(), frame.end());
    payload.push_back(std::byte{0x22});
    for (const auto workers : {1U, 3U}) {
        for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
            const flitzzip::ArchiveCodecOptions options{.worker_count = workers};
            std::array blocks{
                flitzzip::BlockDescriptor{.kind = Kind::Raw, .uncompressed_len = 1, .encoded_len = 1},
                flitzzip::BlockDescriptor{
                    .kind = Kind::Deflate, .uncompressed_len = 1024, .encoded_offset = 1, .encoded_len = 17},
                flitzzip::BlockDescriptor{
                    .kind = Kind::Raw, .uncompressed_len = 1, .encoded_offset = 18, .encoded_len = 1}};
            std::array<std::byte, 1026> output{};
            for (const bool invalid : {false, true, false}) {
                std::ranges::fill(output, std::byte{0x7D});
                blocks[1].encoded_len = invalid ? 18U : 17U;
                bool rejected = false;
                try {
                    decode(payload, blocks, output, options);
                } catch (const flitzzip::ArchiveError& error) {
                    REQUIRE_EQ(std::string(error.what()), "deflate block failed to decompress");
                    rejected = true;
                }
                REQUIRE_EQ(rejected, invalid);
                if (!invalid) {
                    REQUIRE_EQ(std::ranges::count(std::span(output).subspan(1, 1024), std::byte{0}), 1024);
                    const bool whole_chunk = decode == &flitzzip::decode_chunk_cpu;
                    REQUIRE_EQ(output.front(), whole_chunk ? std::byte{0x11} : std::byte{0x7D});
                    REQUIRE_EQ(output.back(), whole_chunk ? std::byte{0x22} : std::byte{0x7D});
                }
            }
        }
    }
}

// Purpose: Reject zero/oversized decoded blocks and unknown kind values before allocating decoder work.
// Inputs: Mutations of one otherwise valid raw record. Outputs: Exact metadata diagnostics and preserved targets.
TEST_CASE(flzip_metadata_rejects_invalid_block_headers) {
    MetadataFixture fixture("native-block-header");
    for (const auto size : {0U, flitzzip::kMaxArchiveBlockBytes + 1U}) {
        auto entry = raw_control();
        entry.blocks[0].uncompressed_len = size;
        entry.uncompressed_size = size;
        write_metadata_fixture(fixture.archive, entry);
        require_metadata_rejection(fixture.archive, fixture.destination,
                                   size == 0 ? "zero decoded length" : "block size limit");
    }
    auto entry = raw_control();
    entry.blocks[0].kind = static_cast<Kind>(255);
    write_metadata_fixture(fixture.archive, entry);
    require_metadata_rejection(fixture.archive, fixture.destination, "unknown encoding kind");
}

// Purpose: Reject invalid compressed-size relationships for every native block representation.
// Inputs: All supported kinds, zero/equal/oversized payload lengths and small bounded wire fixtures.
// Outputs: Both public readers refuse malformed metadata before decoder execution or publication.
TEST_CASE(flzip_metadata_rejects_invalid_payload_lengths_for_all_kinds) {
    MetadataFixture fixture("native-payload-lengths");
    constexpr std::array kinds{Kind::Raw,           Kind::Fill,
                               Kind::Deflate,       Kind::Pattern,
                               Kind::GpuPrefix,     Kind::GpuAdaptivePrefix,
                               Kind::GpuDictionary, Kind::GpuSparsePattern,
                               Kind::CpuZstd,       Kind::GpuLongSparsePattern,
                               Kind::GpuHuffman,    Kind::GpuCompound,
                               Kind::GpuBytePlane,  Kind::GpuBytePlaneContexts};
    const std::string payload(32769, 'x');
    for (const auto kind : kinds) {
        for (const auto length : {0U, 32768U, 32769U}) {
            if ((kind == Kind::Raw && length == 32768U) || (kind == Kind::Fill && length == 0U)) {
                continue;  // These two extent relationships are valid; other tests qualify their actual payloads.
            }
            auto entry = raw_control();
            entry.uncompressed_size = 32768;
            entry.payload_size = length;
            entry.blocks[0] = {.kind = kind, .uncompressed_len = 32768, .encoded_len = length};
            write_metadata_fixture(fixture.archive, entry, payload);
            require_metadata_rejection(fixture.archive, fixture.destination,
                                       kind == Kind::Fill ? "fill block contains payload" : "invalid");
        }
    }
}

// Purpose: Reject holes and overlap independently of codec payload contents.
// Inputs: Valid outer lengths for each payload-bearing kind but nonzero first block offsets.
// Outputs: Both readers report the dense-offset violation before consulting compressed payload bytes.
TEST_CASE(flzip_metadata_rejects_sparse_and_overlapping_payload_offsets) {
    MetadataFixture fixture("native-payload-offsets");
    constexpr std::array<std::pair<Kind, std::uint32_t>, 13> shapes{{std::pair{Kind::Raw, 32768U},
                                                                     {Kind::Deflate, 12U},
                                                                     {Kind::CpuZstd, 12U},
                                                                     {Kind::Pattern, 2U},
                                                                     {Kind::GpuPrefix, 37U},
                                                                     {Kind::GpuAdaptivePrefix, 1024U},
                                                                     {Kind::GpuDictionary, 32U},
                                                                     {Kind::GpuSparsePattern, 64U},
                                                                     {Kind::GpuLongSparsePattern, 64U},
                                                                     {Kind::GpuHuffman, 8230U},
                                                                     {Kind::GpuCompound, 8U},
                                                                     {Kind::GpuBytePlane, 4U},
                                                                     {Kind::GpuBytePlaneContexts, 16U}}};
    const std::string payload(32768, 'x');
    for (const auto& [kind, length] : shapes) {
        for (const auto offset : {1ULL, 0xFFFFFFFFFFFFFFFFULL}) {
            auto entry = raw_control();
            entry.uncompressed_size = 32768;
            entry.payload_size = length;
            entry.blocks[0] = {
                .kind = kind, .uncompressed_len = 32768, .encoded_offset = offset, .encoded_len = length};
            write_metadata_fixture(fixture.archive, entry, payload);
            require_metadata_rejection(fixture.archive, fixture.destination, "sparse or overlapping");
        }
    }
}

// Purpose: Reject file/directory inconsistencies and mismatched aggregate extents before touching destinations.
// Inputs: Otherwise valid raw records with one independently changed aggregate field.
// Outputs: Both verify and extract reject each record at the expected shared metadata boundary.
TEST_CASE(flzip_metadata_rejects_entry_aggregate_inconsistencies) {
    MetadataFixture fixture("native-entry-aggregates");
    for (const bool directory : {false, true}) {
        auto entry = raw_control();
        entry.directory = directory;
        entry.uncompressed_size = 4;
        write_metadata_fixture(fixture.archive, entry);
        require_metadata_rejection(fixture.archive, fixture.destination,
                                   directory ? "directory entry has payload" : "block sizes do not match");
    }
    auto entry = raw_control();
    entry.payload_size = 4;
    write_metadata_fixture(fixture.archive, entry, "foox");
    require_metadata_rejection(fixture.archive, fixture.destination, "payload size does not match");
    entry = raw_control();
    entry.payload_offset = 0xFFFFFFFFFFFFFFFFULL;
    write_metadata_fixture(fixture.archive, entry);
    require_metadata_rejection(fixture.archive, fixture.destination, "payload overlaps index or footer");
}

// Purpose: Prove invalid resource options are rejected by create, extract and verify before filesystem work.
// Inputs: Each public chunk/block/worker/inflight limit violated independently, using a nonexistent source.
// Outputs: Specific option diagnostics, unchanged destination and no published archive.
TEST_CASE(flzip_public_operations_reject_invalid_resource_options_first) {
    struct Limits {
        std::uint64_t chunk;
        std::uint32_t block;
        std::uint32_t workers;
        std::uint32_t inflight;
        const char* diagnostic;
    };
    constexpr std::array<Limits, 8> cases{
        {Limits{0, 4096, 1, 1, "chunk size must"},
         {flitzzip::kMaxArchiveChunkBytes + 1, 4096, 1, 1, "chunk size exceeds"},
         {8192, 0, 1, 1, "block size is outside"},
         {8192, flitzzip::kMaxArchiveBlockBytes + 1, 1, 1, "block size is outside"},
         {4096, 8192, 1, 1, "block size cannot exceed"},
         {8193, 4096, 1, 1, "exact multiple"},
         {8192, 4096, flitzzip::kMaxArchiveWorkers + 1, 1, "worker count exceeds"},
         {8192, 4096, 1, flitzzip::kMaxInflightArchiveChunks + 1, "in-flight chunk count exceeds"}}};
    MetadataFixture fixture("native-option-limits");
    for (const auto& limits : cases) {
        for (const int operation : {0, 1, 2}) {
            bool rejected = false;
            try {
                const flitzzip::ExtractOptions extract{.gpu_required = false,
                                                       .force_cpu = true,
                                                       .overwrite = true,
                                                       .chunk_size = limits.chunk,
                                                       .block_size = limits.block,
                                                       .worker_count = limits.workers,
                                                       .max_inflight_chunks = limits.inflight};
                if (operation == 0) {
                    flitzzip::CompressOptions create{.gpu_required = false,
                                                     .force_cpu = true,
                                                     .chunk_size = limits.chunk,
                                                     .block_size = limits.block,
                                                     .worker_count = limits.workers,
                                                     .max_inflight_chunks = limits.inflight};
                    (void)flitzzip::compress_flzip({fixture.root / "missing"}, fixture.archive, create);
                } else if (operation == 1) {
                    (void)flitzzip::extract_flzip(fixture.archive, fixture.destination, extract);
                } else {
                    (void)flitzzip::verify_flzip(fixture.archive, extract);
                }
            } catch (const flitzzip::ArchiveError& error) {
                REQUIRE_TRUE(std::string_view(error.what()).find(limits.diagnostic) != std::string_view::npos);
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(!std::filesystem::exists(fixture.archive));
        }
    }
}

// Purpose: Reject a zero-length descriptor even when an internal CPU caller supplies an empty output span.
// Inputs: Both CPU decode entry points, one empty fill descriptor, and a genuinely empty chunk as the positive control.
// Outputs: The malformed block is rejected while the empty chunk remains usable.
TEST_CASE(flzip_cpu_decode_rejects_zero_length_blocks) {
    const flitzzip::BlockDescriptor empty_fill{.kind = Kind::Fill};
    for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
        decode({}, {}, {}, {});
        bool rejected = false;
        try {
            decode({}, std::span(&empty_fill, 1), {}, {});
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "decode block length is outside FlitzZip resource limits");
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        decode({}, {}, {}, {});
    }
}

// Purpose: Enforce the wire block-size limit at CPU decode admission, independently of the enclosing buffer capacity.
// Inputs: A bounded RAM-only fill block exactly at the limit and another one byte beyond it.
// Outputs: Both CPU consumers reject the oversized block before modifying output, then accept the boundary control.
TEST_CASE(flzip_cpu_decode_rejects_oversized_blocks_before_output) {
    std::vector<std::byte> output(flitzzip::kMaxArchiveBlockBytes + 1U, std::byte{0x7D});
    for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
        flitzzip::BlockDescriptor fill{
            .kind = Kind::Fill, .fill_value = 37U, .uncompressed_len = flitzzip::kMaxArchiveBlockBytes + 1U};
        std::ranges::fill(output, std::byte{0x7D});
        bool rejected = false;
        try {
            decode({}, std::span(&fill, 1), output, {});
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "decode block length is outside FlitzZip resource limits");
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), static_cast<std::ptrdiff_t>(output.size()));
        fill.uncompressed_len = flitzzip::kMaxArchiveBlockBytes;
        decode({}, std::span(&fill, 1), std::span(output).first(flitzzip::kMaxArchiveBlockBytes), {});
        const auto expected = decode == &flitzzip::decode_chunk_cpu ? std::byte{37} : std::byte{0x7D};
        REQUIRE_EQ(std::ranges::count(std::span(output).first(flitzzip::kMaxArchiveBlockBytes), expected),
                   static_cast<std::ptrdiff_t>(flitzzip::kMaxArchiveBlockBytes));
        REQUIRE_EQ(output.back(), std::byte{0x7D});
    }
}

// Purpose: Reject malformed block extents before either CPU consumer writes any block in the chunk.
// Inputs: A valid raw block followed by independently invalid length/kind descriptors and a raw/fill positive control.
// Outputs: Rejection leaves the entire destination unchanged, and valid decoding still respects each consumer's
// ownership.
TEST_CASE(flzip_cpu_layout_admission_preserves_output) {
    constexpr std::array cases{
        std::pair{Kind::Raw, 1U},
        std::pair{Kind::Fill, 1U},
        std::pair{Kind::Deflate, 4096U},
        std::pair{Kind::CpuZstd, 4096U},
        std::pair{Kind::Pattern, 1U},
        std::pair{Kind::GpuPrefix, 1U},
        std::pair{Kind::GpuAdaptivePrefix, 1U},
        std::pair{Kind::GpuDictionary, 1U},
        std::pair{Kind::GpuHuffman, 1U},
        std::pair{static_cast<Kind>(255U), 0U},
    };
    std::array<std::byte, 4096> payload{};
    std::array<std::byte, 8192> output{};
    for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
        const flitzzip::ArchiveCodecOptions options{.worker_count = 2};
        for (const auto& [kind, encoded] : cases) {
            const std::array blocks{
                flitzzip::BlockDescriptor{.kind = Kind::Raw, .uncompressed_len = 4096, .encoded_len = 4096},
                flitzzip::BlockDescriptor{.kind = kind, .uncompressed_len = 4096, .encoded_len = encoded}};
            std::ranges::fill(output, std::byte{0x7D});
            bool rejected = false;
            try {
                decode(payload, blocks, output, options);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), static_cast<std::ptrdiff_t>(output.size()));
        }
        const std::array valid{
            flitzzip::BlockDescriptor{.kind = Kind::Raw, .uncompressed_len = 4096, .encoded_len = 4096},
            flitzzip::BlockDescriptor{.kind = Kind::Fill, .fill_value = 37, .uncompressed_len = 4096}};
        decode(payload, valid, output, options);
        const bool whole_chunk = decode == &flitzzip::decode_chunk_cpu;
        REQUIRE_EQ(std::ranges::count(std::span(output).first(4096), whole_chunk ? std::byte{0} : std::byte{0x7D}),
                   4096);
        REQUIRE_EQ(std::ranges::count(std::span(output).last(4096), whole_chunk ? std::byte{37} : std::byte{0x7D}),
                   4096);
    }
}

namespace {

// Purpose: Require the intended codec admission error rather than accepting any unrelated exception.
// Inputs: A bounded operation and its specified diagnostic. Outputs: Fails if the operation succeeds or differs.
template <typename Operation> void require_codec_rejection(Operation operation, std::string_view diagnostic) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Enforce codec option bounds even when an internal caller bypasses public archive admission.
// Inputs: Independent block-size and effort boundaries, immutable source bytes and canary-filled output.
// Outputs: Invalid options fail before output; a subsequent valid encode/decode returns exact source bytes.
TEST_CASE(flzip_cpu_codec_options_reject_before_work_and_recover) {
    constexpr std::array payload{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    const flitzzip::BlockDescriptor raw{.kind = Kind::Raw, .uncompressed_len = 4U, .encoded_len = 4U};
    std::array<std::byte, 4> output{};
    for (const auto block_size : {0U, flitzzip::kMinArchiveBlockBytes - 1U, flitzzip::kMaxArchiveBlockBytes + 1U,
                                  std::numeric_limits<std::uint32_t>::max()}) {
        const flitzzip::ArchiveCodecOptions options{.block_size = block_size};
        require_codec_rejection([&] { (void)flitzzip::encode_chunk_cpu(payload, options); },
                                "codec block size is outside");
        for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
            output.fill(std::byte{0x7D});
            require_codec_rejection([&] { decode(payload, std::span(&raw, 1U), output, options); },
                                    "codec block size is outside");
            REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), 4);
        }
        const flitzzip::GpuCodecOptions dispatch{.force_cpu = true, .block_size = block_size};
        require_codec_rejection([&] { (void)flitzzip::encode_chunk(payload, dispatch); },
                                "codec block size is outside");
        output.fill(std::byte{0x7D});
        require_codec_rejection([&] { (void)flitzzip::decode_chunk(payload, std::span(&raw, 1U), output, dispatch); },
                                "codec block size is outside");
        REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), 4);
    }
    for (const int effort : {std::numeric_limits<int>::min(), 0, 10, std::numeric_limits<int>::max()}) {
        require_codec_rejection([&] { (void)flitzzip::encode_chunk_cpu(payload, {.compression_level = effort}); },
                                "codec compression level must be between 1 and 9");
    }
    const auto encoded = flitzzip::encode_chunk_cpu(payload, {});
    flitzzip::decode_chunk_cpu(encoded.payload, encoded.blocks, output, {});
    REQUIRE_EQ(output, payload);
}

// Purpose: Test the shared codec span limit numerically without allocating a deliberately rejected giant buffer.
// Inputs: Exact block/chunk boundaries and each adjacent invalid value. Outputs: One diagnostic contract for CPU/HIP.
TEST_CASE(flzip_shared_codec_resource_limits_have_exact_boundaries) {
    flitzzip::validate_codec_block_size(flitzzip::kMinArchiveBlockBytes);
    flitzzip::validate_codec_block_size(flitzzip::kMaxArchiveBlockBytes);
    flitzzip::validate_codec_span_size(static_cast<std::size_t>(flitzzip::kMaxArchiveChunkBytes), "test input");
    require_codec_rejection([] { flitzzip::validate_codec_block_size(flitzzip::kMinArchiveBlockBytes - 1U); },
                            "codec block size is outside");
    require_codec_rejection([] { flitzzip::validate_codec_block_size(flitzzip::kMaxArchiveBlockBytes + 1U); },
                            "codec block size is outside");
    require_codec_rejection(
        [] {
            flitzzip::validate_codec_span_size(static_cast<std::size_t>(flitzzip::kMaxArchiveChunkBytes + 1U),
                                               "test input");
        },
        "test input exceeds FlitzZip codec resource limit");
}

// Purpose: Reject caller output extents and excessive tables before parallel decoding can publish a prefix.
// Inputs: A four-byte raw block, shorter/longer destinations and a genuinely allocated over-limit table.
// Outputs: Both CPU consumers preserve all canaries on rejection and remain usable for a valid next decode.
TEST_CASE(flzip_cpu_decode_extent_admission_preserves_output_and_recovers) {
    constexpr std::array payload{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    const flitzzip::BlockDescriptor raw{.kind = Kind::Raw, .uncompressed_len = 4U, .encoded_len = 4U};
    const std::vector<flitzzip::BlockDescriptor> excessive(flitzzip::kMaxBlocksPerEntry + 1ULL);
    std::array<std::byte, 8> output{};
    for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
        for (const std::size_t length : {2U, 5U}) {
            output.fill(std::byte{0x7D});
            require_codec_rejection(
                [&] { decode(payload, std::span(&raw, 1U), std::span(output).subspan(1U, length), {}); },
                length < 4U ? "decoded block exceeds output buffer" : "decoded size does not match");
            REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), 8);
        }
        require_codec_rejection([&] { decode({}, excessive, output, {}); }, "codec block table exceeds");
        REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), 8);
        decode(payload, std::span(&raw, 1U), std::span(output).subspan(1U, 4U), {});
        REQUIRE_EQ(output.front(), std::byte{0x7D});
        REQUIRE_EQ(std::ranges::count(std::span(output).last(3U), std::byte{0x7D}), 3);
        if (decode == &flitzzip::decode_chunk_cpu) {
            REQUIRE_TRUE(std::ranges::equal(std::span(output).subspan(1U, 4U), payload));
        } else {
            REQUIRE_EQ(std::ranges::count(output, std::byte{0x7D}), 8);
        }
    }
    const flitzzip::GpuCodecOptions dispatch{.force_cpu = true};
    require_codec_rejection([&] { (void)flitzzip::decode_chunk({}, excessive, output, dispatch); },
                            "codec block table exceeds");
    require_codec_rejection([&] { (void)flitzzip::crc_decoded_chunk({}, excessive, 0U, dispatch); },
                            "codec block table exceeds");
    require_codec_rejection(
        [&] { (void)flitzzip::crc_decoded_chunk({}, {}, flitzzip::kMaxArchiveChunkBytes + 1U, dispatch); },
        "codec output exceeds FlitzZip resource limit");
}

// Purpose: Guard HIP's final host admission even when an internal caller bypasses the public archive parser.
// Inputs: Independent raw/fill controls and malformed extents for every payload-bearing native block kind.
// Outputs: Valid controls pass; truncation, overflow, unknown kinds and output mismatches fail without GPU dispatch.
TEST_CASE(archive_metadata_hip_layout_rejects_untrusted_extents) {
#if FLITZZIP_ENABLE_HIP
    using flitzzip::BlockDescriptor;
    using flitzzip::BlockKind;
    using flitzzip::hip_detail::validate_decode_layout;
    std::array<std::byte, 4096> payload{};
    BlockDescriptor raw{.kind = BlockKind::Raw, .uncompressed_len = 4096U, .encoded_len = 4096U};
    BlockDescriptor fill{.kind = BlockKind::Fill, .fill_value = 37U, .uncompressed_len = 4096U};
    validate_decode_layout(payload, std::span(&raw, 1U), 4096U, 0U);
    validate_decode_layout({}, std::span(&fill, 1U), 4096U, 0U);
    validate_decode_layout({}, {}, 0U, 0U);

    // Purpose: Require a controlled admission error before any kernel or output buffer exists.
    // Inputs: One hostile descriptor table and declared output length. Outputs: Assertion on missing rejection.
    const auto reject = [&](std::span<const BlockDescriptor> blocks, std::size_t output_bytes) {
        bool rejected = false;
        try {
            validate_decode_layout(payload, blocks, output_bytes, 0U);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    };
    reject({}, 1U);
    reject(std::span(&raw, 1U), 4095U);
    reject(std::span(&raw, 1U), 4097U);
    auto changed = raw;
    changed.uncompressed_len = 0U;
    reject(std::span(&changed, 1U), 0U);
    changed = raw;
    --changed.encoded_len;
    reject(std::span(&changed, 1U), 4096U);
    changed = fill;
    changed.encoded_len = 1U;
    reject(std::span(&changed, 1U), 4096U);
    changed = raw;
    changed.kind = static_cast<BlockKind>(255U);
    reject(std::span(&changed, 1U), 4096U);
    for (const auto kind :
         {BlockKind::Raw, BlockKind::Deflate, BlockKind::Pattern, BlockKind::GpuPrefix, BlockKind::GpuAdaptivePrefix,
          BlockKind::GpuDictionary, BlockKind::GpuSparsePattern, BlockKind::CpuZstd, BlockKind::GpuLongSparsePattern,
          BlockKind::GpuHuffman, BlockKind::GpuCompound, BlockKind::GpuBytePlane, BlockKind::GpuBytePlaneContexts}) {
        const auto encoded = kind == BlockKind::Raw ? 4096U : kind == BlockKind::Pattern ? 8U : 512U;
        for (const std::uint64_t offset : {4097ULL - encoded, 4097ULL, std::numeric_limits<std::uint64_t>::max()}) {
            const BlockDescriptor invalid{
                .kind = kind, .uncompressed_len = 4096U, .encoded_offset = offset, .encoded_len = encoded};
            reject(std::span(&invalid, 1U), 4096U);
        }
    }
    for (const auto kind : {BlockKind::Deflate, BlockKind::CpuZstd}) {
        const BlockDescriptor empty{.kind = kind, .uncompressed_len = 4096U};
        reject(std::span(&empty, 1U), 4096U);
    }
#endif
}
