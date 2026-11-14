#pragma once

#include "test_util.hpp"
#include "core/progress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>

namespace flitzzip_test {

// Check observable two-pass archive phases independently of translated status text.
// The caller owns this probe throughout its synchronous extraction callback.
struct ArchivePhaseProbe {
    bool validation_seen = false;
    bool finishing_seen = false;

    void operator()(const flitzzip::ProgressSnapshot& snapshot) {
        REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Extract);
        if (snapshot.note.empty()) {
            return;
        }
        if (snapshot.total_bytes == 0U && snapshot.total_entries == 0U) {
            REQUIRE_EQ(snapshot.processed_bytes, 0U);
            REQUIRE_EQ(snapshot.completed_entries, 0U);
            validation_seen = true;
        } else {
            REQUIRE_EQ(snapshot.processed_bytes, snapshot.total_bytes);
            REQUIRE_EQ(snapshot.completed_entries, snapshot.total_entries);
            finishing_seen = true;
        }
    }
};

// Independent bytes for the frozen liblzma frames in the XZ and lzip tests.
// The payload spans three decoder windows and includes every byte value.
inline std::string decoded_transfer_fixture_payload() {
    std::string result(3U * 65536U + 17U, '\0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<char>(index & 255U);
    }
    return result;
}

// A valid sink whose inherited overflow refuses every byte, without host I/O.
class RefusingDecodedFixtureSink final : public std::streambuf {
  public:
    RefusingDecodedFixtureSink() = default;
};

// Mix standard reads/putback with bounded direct windows. Independent byte counts
// reject a reader that reports progress without consuming its current window.
template <class Reader>
void append_decoded_fixture_windows(Reader& input, std::ostringstream& output, std::size_t expected_bytes,
                                    std::size_t initial_bytes = 0U) {
    constexpr std::array<std::size_t, 4> limits{1U, 17U, 65536U, std::numeric_limits<std::size_t>::max()};
    std::size_t call = 0U;
    std::size_t transferred = initial_bytes;
    REQUIRE_TRUE(transferred <= expected_bytes);
    for (;;) {
        const auto limit = limits[call++ % limits.size()];
        const auto old_count = input.gcount();
        const auto count = input.transfer_to(output, limit);
        REQUIRE_TRUE(count <= limit);
        REQUIRE_EQ(input.gcount(), old_count);
        REQUIRE_TRUE(count <= expected_bytes - transferred);
        transferred += count;
        if (count == 0U) {
            break;
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            char next = 0;
            input.get(next);
            input.unget();
            REQUIRE_TRUE(input.good());
        }
    }
    REQUIRE_EQ(transferred, expected_bytes);
}

// Exercise real read-only codec facades against independently encoded frames.
// Exact bytes, counters, partial finish and failed-write recovery are mandatory;
// no encoder, synthetic timing, benchmark workload or runtime tool is required.
template <class Reader>
void require_decoded_fixture_transfer(const std::filesystem::path& archive, std::string_view expected) {
    {
        Reader input(archive);
        std::ostringstream output;
        REQUIRE_EQ(input.transfer_to(output, 0U), 0U);
        REQUIRE_EQ(input.output_bytes(), 0U);
        std::array<char, 7> prefix{};
        const auto prefix_bytes = std::min(prefix.size(), expected.size());
        input.read(prefix.data(), static_cast<std::streamsize>(prefix_bytes));
        REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(prefix_bytes));
        output.write(prefix.data(), static_cast<std::streamsize>(prefix_bytes));
        append_decoded_fixture_windows(input, output, expected.size(), prefix_bytes);
        input.finish();
        REQUIRE_EQ(output.str(), expected);
        REQUIRE_EQ(input.output_bytes(), expected.size());
        REQUIRE_EQ(input.input_bytes(), std::filesystem::file_size(archive));
    }
    {
        Reader input(archive);
        std::ostringstream prefix;
        const auto prefix_bytes = std::min<std::size_t>(13U, expected.size());
        const auto written = input.transfer_to(prefix, prefix_bytes);
        REQUIRE_TRUE(written <= prefix_bytes);
        input.finish();
        input.finish();
        REQUIRE_EQ(prefix.str(), expected.substr(0U, written));
        REQUIRE_EQ(input.output_bytes(), expected.size());
        REQUIRE_EQ(input.transfer_to(prefix, 65536U), 0U);
    }
    {
        Reader input(archive);
        RefusingDecodedFixtureSink sink;
        std::ostream refused(&sink);
        if (!expected.empty()) {
            bool failed = false;
            try {
                static_cast<void>(input.transfer_to(refused, 17U));
            } catch (const std::ios_base::failure&) {
                failed = true;
            }
            REQUIRE_TRUE(failed && refused.bad());
        }
        std::ostringstream recovered;
        append_decoded_fixture_windows(input, recovered, expected.size());
        input.finish();
        REQUIRE_EQ(recovered.str(), expected);
        REQUIRE_EQ(input.output_bytes(), expected.size());
    }
}

}  // namespace flitzzip_test
