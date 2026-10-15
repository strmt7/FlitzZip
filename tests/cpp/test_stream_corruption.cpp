#include "test_util.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"
#include "zstd/zstd_stream.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <span>

namespace {

// Purpose: Replace one bounded owned fixture while ensuring no stale file handle overlaps the next reader.
// Inputs: Temporary path and exact bytes. Outputs: Complete binary file, or a test failure on I/O error.
void write_corrupt_stream(const std::filesystem::path& path, std::span<const char> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

// Purpose: Ensure damage is rejected whether validation starts with a read or explicit finalization.
// Inputs: A malformed stream file and the public input-stream type. Outputs: Repeated failure survives state reset.
template <typename Input> void require_terminal_stream_damage(const std::filesystem::path& path) {
    for (const bool read_first : {false, true}) {
        bool rejected = false;
        try {
            Input input(path);
            try {
                if (read_first) {
                    std::array<char, 1024> scratch{};
                    input.read(scratch.data(), static_cast<std::streamsize>(scratch.size()));
                }
                input.finish();
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
                input.clear();
                bool rejected_again = false;
                try {
                    input.finish();
                } catch (const flitzzip::ArchiveError&) {
                    rejected_again = true;
                }
                REQUIRE_TRUE(rejected_again);
            }
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;  // Header rejection at construction is also a terminal failure.
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Detect premature successful EOF anywhere in a checksummed stream, not just its final trailer byte.
// Inputs: A public encoder/decoder pair and a bounded 257-byte payload containing every byte value.
// Outputs: Every strict frame prefix and trailing garbage fail; full frames remain usable afterward.
template <typename Output, typename Input> void require_all_truncations_rejected(const char* label) {
    const auto root = test_temp_dir(label);
    const auto path = root / "frame";
    std::string payload;
    for (unsigned int value = 0; value <= 256U; ++value) {
        payload.push_back(static_cast<char>(value & 255U));
    }
    {
        Output output(path, 5);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.close();
    }
    std::vector<char> frame;
    {
        std::ifstream source(path, std::ios::binary);
        frame.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
    }
    REQUIRE_TRUE(!frame.empty() && frame.size() < 1024U);
    for (std::size_t cut = 0; cut < frame.size(); ++cut) {
        write_corrupt_stream(path, std::span(frame).first(cut));
        require_terminal_stream_damage<Input>(path);
    }
    auto garbage = frame;
    garbage.push_back('x');
    write_corrupt_stream(path, garbage);
    require_terminal_stream_damage<Input>(path);
    write_corrupt_stream(path, frame);
    {
        Input input(path);
        std::string restored;
        for (std::size_t index = 0; index < payload.size(); ++index) {
            const auto next = input.peek();
            REQUIRE_EQ(next, static_cast<unsigned char>(payload[index]));
            REQUIRE_EQ(input.get(), next);
            restored.push_back(static_cast<char>(next));
        }
        REQUIRE_EQ(input.peek(), std::char_traits<char>::eof());
        input.finish();
        input.finish();
        REQUIRE_EQ(restored, payload);
        REQUIRE_EQ(input.input_bytes(), frame.size());
        REQUIRE_EQ(input.output_bytes(), payload.size());
    }
    // Finalization must validate unread output as well as output consumed by a caller.
    {
        Input input(path);
        REQUIRE_EQ(input.get(), 0);
        input.finish();
        REQUIRE_EQ(input.output_bytes(), payload.size());
    }
    std::filesystem::remove_all(root);
}

}  // namespace

// Purpose: Reject incomplete Gzip members at every byte boundary with stable error state.
// Inputs: Bounded independently varied cuts of a valid member. Outputs: Complete content alone is accepted.
TEST_CASE(stream_corruption_gzip_every_truncation_is_terminal) {
    require_all_truncations_rejected<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("gzip-all-truncations");
}

// Purpose: Reject incomplete Bzip2 members at every byte boundary with stable error state.
// Inputs: Bounded independently varied cuts of a valid member. Outputs: Complete content alone is accepted.
TEST_CASE(stream_corruption_bzip2_every_truncation_is_terminal) {
    require_all_truncations_rejected<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>("bzip2-all-truncations");
}

// Purpose: Reject incomplete Zstandard frames at every byte boundary with stable error state.
// Inputs: Bounded independently varied cuts of a valid frame. Outputs: Complete content alone is accepted.
TEST_CASE(stream_corruption_zstd_every_truncation_is_terminal) {
    require_all_truncations_rejected<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("zstd-all-truncations");
}
