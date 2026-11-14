#pragma once

#include "test_flzip_helpers.hpp"
#include "test_source_session.hpp"

namespace flitzzip_test {

inline std::string stream_cancellation_payload(std::size_t bytes = 3U * 65536U + 17U) {
    std::string result(bytes, '\0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<char>((index * 37U + index / 251U) & 0xFFU);
    }
    return result;
}

// Own a small multi-window fixture for one synchronous contract. The factory
// receives exact independent bytes; neither factory nor check retains references.
template <class Factory, class Check>
void with_stream_cancellation_fixture(const char* name, Factory factory, Check check) {
    ScopedTestDirectory cleanup{test_temp_dir(name)};
    const auto expected = stream_cancellation_payload(512U * 1024U + 17U);
    const auto archive = factory(cleanup.path, expected);
    check(archive, cleanup.path / "output", expected);
    std::filesystem::remove_all(cleanup.path);
}

// Preparation has no trusted publication totals. It must not claim extracted
// bytes or completed entries; callers separately observe publication progress.
inline bool is_prepublication_progress(const flitzzip::ProgressSnapshot& snapshot) {
    if (snapshot.total_entries != 0U) {
        return false;
    }
    REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Extract);
    REQUIRE_EQ(snapshot.total_bytes, 0U);
    REQUIRE_EQ(snapshot.processed_bytes, 0U);
    REQUIRE_EQ(snapshot.completed_entries, 0U);
    return true;
}

inline void require_stream_fixture_readback(const std::filesystem::path& archive,
                                            const std::filesystem::path& destination, std::string_view expected,
                                            ArchiveExtractor extract) {
    std::uint64_t observed_bytes = 0U;
    std::uint64_t observed_entries = 0U;
    const auto stats = extract(archive, destination, false, [&](const auto& snapshot) {
        if (!is_prepublication_progress(snapshot)) {
            REQUIRE_EQ(snapshot.total_bytes, expected.size());
            REQUIRE_EQ(snapshot.total_entries, 1U);
            REQUIRE_TRUE(snapshot.processed_bytes >= observed_bytes &&
                         snapshot.processed_bytes <= snapshot.total_bytes);
            REQUIRE_TRUE(snapshot.completed_entries >= observed_entries && snapshot.completed_entries <= 1U);
            observed_bytes = snapshot.processed_bytes;
            observed_entries = snapshot.completed_entries;
        }
    });
    REQUIRE_EQ(stats.input_bytes, std::filesystem::file_size(archive));
    REQUIRE_EQ(stats.output_bytes, expected.size());
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(observed_bytes, expected.size());
    REQUIRE_EQ(observed_entries, 1U);
    REQUIRE_EQ(source_session_fixture_bytes(destination / "payload.bin"), expected);
}

// A multi-window valid stream must admit cancellation before final validation or
// publication, preserve caller data, release its source and allow a clean retry.
// Count checkpoints instead of sleeping or depending on decoder speed.
// Payload-byte adapters can additionally require interruption strictly between
// their first successful write and final byte; compressed-input counters retain
// the ordinary checkpoint-count contract. Inspect the original stateful target.
inline void require_stream_window_cancellation(const std::filesystem::path& archive,
                                               const std::filesystem::path& destination, std::string_view member,
                                               std::string_view expected, ArchiveExtractor extract,
                                               bool require_partial_progress = false) {
    const auto original_archive = source_session_fixture_bytes(archive);
    const auto target = destination / member;
    std::filesystem::create_directories(destination);
    for (const bool existing : {false, true}) {
        if (existing) {
            std::ofstream output(target, std::ios::binary);
            output << "preserved caller bytes";
        }
        struct WindowObserver {
            bool partial;
            std::size_t cancel_after;
            std::size_t checkpoints = 0U;

            void operator()(const flitzzip::ProgressSnapshot& snapshot) {
                REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Extract);
                REQUIRE_TRUE(snapshot.processed_bytes <= snapshot.total_bytes);
                const bool eligible =
                    snapshot.total_entries != 0U && snapshot.completed_entries == 0U &&
                    (!partial || (snapshot.processed_bytes > 0U && snapshot.processed_bytes < snapshot.total_bytes));
                if (eligible && ++checkpoints == cancel_after) {
                    throw SourceSessionInterrupted();
                }
            }
        };
        const std::size_t cancel_after = require_partial_progress ? 1U : 3U;
        const flitzzip::ProgressCallback observer = WindowObserver{require_partial_progress, cancel_after};
        bool interrupted = false;
        try {
            (void)extract(archive, destination, true, observer);
        } catch (const SourceSessionInterrupted&) {
            interrupted = true;
        }
        REQUIRE_TRUE(interrupted);
        REQUIRE_TRUE(observer.target<WindowObserver>() != nullptr);
        REQUIRE_EQ(observer.target<WindowObserver>()->checkpoints, cancel_after);
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator()),
            existing ? 1 : 0);
        REQUIRE_EQ(std::filesystem::exists(target), existing);
        if (existing) {
            REQUIRE_EQ(source_session_fixture_bytes(target), "preserved caller bytes");
        }
        REQUIRE_EQ(source_session_fixture_bytes(archive), original_archive);
        DWORD error = ERROR_SUCCESS;
        REQUIRE_TRUE(source_writer_available(archive, error));
    }
    const auto stats = extract(archive, destination, true, {});
    REQUIRE_EQ(stats.input_bytes, original_archive.size());
    REQUIRE_EQ(stats.output_bytes, expected.size());
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(source_session_fixture_bytes(target), expected);
    REQUIRE_EQ(source_session_fixture_bytes(archive), original_archive);
}

// Validation precedes trusted totals and destination changes. Cancellation must
// preserve an existing target, release the source and permit a complete retry.
inline void require_validation_cancellation(const std::filesystem::path& archive,
                                            const std::filesystem::path& destination, std::string_view member,
                                            std::string_view expected, ArchiveExtractor extract,
                                            std::size_t cancel_after = 5U, std::string_view phase_note = {}) {
    std::filesystem::create_directories(destination);
    const auto target = destination / member;
    {
        std::ofstream output(target, std::ios::binary);
        output << "preserved validation target";
    }
    const auto original = source_session_fixture_bytes(archive);
    struct ValidationObserver {
        std::size_t cancel_after;
        std::size_t checkpoints = 0U;
        std::string_view phase_note;

        void operator()(const flitzzip::ProgressSnapshot& snapshot) {
            if (snapshot.total_bytes == 0U && snapshot.total_entries == 0U &&
                (phase_note.empty() || snapshot.note == phase_note)) {
                REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Extract);
                REQUIRE_EQ(snapshot.processed_bytes, 0U);
                REQUIRE_EQ(snapshot.completed_entries, 0U);
                if (++checkpoints == cancel_after) {
                    throw SourceSessionInterrupted();
                }
            }
        }
    };
    const flitzzip::ProgressCallback observer = ValidationObserver{cancel_after, 0U, phase_note};
    bool interrupted = false;
    try {
        (void)extract(archive, destination, true, observer);
    } catch (const SourceSessionInterrupted&) {
        interrupted = true;
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_TRUE(observer.target<ValidationObserver>() != nullptr);
    REQUIRE_EQ(observer.target<ValidationObserver>()->checkpoints, cancel_after);
    REQUIRE_EQ(source_session_fixture_bytes(target), "preserved validation target");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator()),
               1);
    DWORD error = ERROR_SUCCESS;
    REQUIRE_TRUE(source_writer_available(archive, error));
    REQUIRE_EQ(source_session_fixture_bytes(archive), original);
    const auto stats = extract(archive, destination, true, {});
    REQUIRE_EQ(stats.input_bytes, original.size());
    REQUIRE_EQ(stats.output_bytes, expected.size());
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(source_session_fixture_bytes(target), expected);
}

}  // namespace flitzzip_test
