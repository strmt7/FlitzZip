#pragma once

#include "test_stream_progress.hpp"

namespace flitzzip_test {

enum class TextTransferCheck {
    Readback,
    PayloadCancellation,
    TrailingCancellation,
    FinalCounters,
    BeforePublication,
    BoundedCallbacks
};

// Independent known encodings of the ASCII bytes ABC are supplied by each
// format. Line endings and all framing remain fixture bytes, not product output.
inline std::string make_literal_transfer_text(std::string_view payload, std::string_view newline,
                                              std::string_view prefix, std::string_view record,
                                              std::string_view suffix) {
    REQUIRE_TRUE(payload.size() % 3U == 0U);
    std::string result(prefix);
    for (std::size_t offset = 0; offset < payload.size(); offset += 3U) {
        REQUIRE_EQ(payload.substr(offset, 3U), "ABC");
        result.append(record);
        result.append(newline);
    }
    result.append(suffix);
    return result;
}

inline void require_text_transfer_readback(const std::filesystem::path& archive, const std::filesystem::path& output,
                                           std::string_view payload, ArchiveExtractor extract) {
    const auto stats = extract(archive, output, true, {});
    REQUIRE_EQ(stats.input_bytes, std::filesystem::file_size(archive));
    REQUIRE_EQ(stats.output_bytes, payload.size());
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(source_session_fixture_bytes(output / "payload.bin"), payload);
    std::filesystem::remove_all(output);
}

// Observe encoded input work independently of decoded output length. Interrupt
// only the requested payload/trailer/publication boundary, preserving the
// original stateful callback and its exact exception across the public adapter.
inline void require_text_transfer_interruption(const std::filesystem::path& archive,
                                               const std::filesystem::path& output, std::string_view payload,
                                               ArchiveExtractor extract, TextTransferCheck check,
                                               std::uint64_t trailer_begin) {
    const auto original = source_session_fixture_bytes(archive);
    const auto target = output / "payload.bin";
    for (const bool existing : {false, true}) {
        std::filesystem::create_directories(output);
        if (existing) {
            std::ofstream stream(target, std::ios::binary);
            stream << "preserved transfer target";
        }
        struct Observer {
            TextTransferCheck check;
            std::uint64_t total;
            std::uint64_t trailer_begin;
            std::size_t interrupted = 0;
            void operator()(const flitzzip::ProgressSnapshot& snapshot) {
                if (is_prepublication_progress(snapshot)) {
                    return;
                }
                REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Extract);
                REQUIRE_EQ(snapshot.total_bytes, total);
                REQUIRE_TRUE(snapshot.processed_bytes <= total);
                if (snapshot.completed_entries != 0U) {
                    return;
                }
                const bool eligible =
                    check == TextTransferCheck::BeforePublication
                        ? snapshot.processed_bytes == total
                        : snapshot.processed_bytes > trailer_begin + 65536U && snapshot.processed_bytes < total;
                if (eligible) {
                    ++interrupted;
                    throw SourceSessionInterrupted();
                }
            }
        };
        const flitzzip::ProgressCallback observer = Observer{check, original.size(), trailer_begin};
        bool interrupted = false;
        try {
            (void)extract(archive, output, true, observer);
        } catch (const SourceSessionInterrupted&) {
            interrupted = true;
        }
        REQUIRE_TRUE(interrupted);
        REQUIRE_TRUE(observer.target<Observer>() != nullptr);
        REQUIRE_EQ(observer.target<Observer>()->interrupted, 1U);
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}),
                   existing ? 1 : 0);
        REQUIRE_EQ(std::filesystem::exists(target), existing);
        if (existing) {
            REQUIRE_EQ(source_session_fixture_bytes(target), "preserved transfer target");
        }
        REQUIRE_EQ(source_session_fixture_bytes(archive), original);
        DWORD error = ERROR_SUCCESS;
        REQUIRE_TRUE(source_writer_available(archive, error));
        require_text_transfer_readback(archive, output, payload, extract);
    }
}

// Batch line-ending, progress, cancellation and recovery controls around a real
// public adapter. Fixtures are bounded below one MiB and use no timing oracle.
template <class Encode>
void require_text_transfer_progress(const char* name, const char* extension, Encode encode, ArchiveExtractor extract,
                                    TextTransferCheck check) {
    ScopedTestDirectory cleanup{test_temp_dir(name)};
    const auto archive = cleanup.path / (std::string("payload.bin.") + extension);
    const auto output = cleanup.path / "output";
    std::string payload;
    const auto repetitions =
        check == TextTransferCheck::PayloadCancellation || check == TextTransferCheck::BoundedCallbacks ? 65536U : 1U;
    for (unsigned index = 0; index < repetitions; ++index) {
        payload += "ABC";
    }
    for (const std::string_view newline : {std::string_view("\n"), std::string_view("\r\n")}) {
        auto text = encode(payload, newline);
        const auto trailer_begin = text.size();
        if (check == TextTransferCheck::TrailingCancellation) {
            for (unsigned index = 0; index < 512U; ++index) {
                text.append(512U, ' ');
                text.append(newline);
            }
        }
        {
            std::ofstream stream(archive, std::ios::binary);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE_TRUE(stream.good());
        }
        require_text_transfer_readback(archive, output, payload, extract);
        if (check == TextTransferCheck::PayloadCancellation) {
            require_stream_window_cancellation(archive, output, "payload.bin", payload, extract, true);
            std::filesystem::remove_all(output);
        } else if (check == TextTransferCheck::FinalCounters || check == TextTransferCheck::BoundedCallbacks) {
            flitzzip::ProgressSnapshot last;
            std::size_t callbacks = 0;
            (void)extract(archive, output, false, [&](const auto& snapshot) {
                ++callbacks;
                REQUIRE_TRUE(snapshot.processed_bytes >= last.processed_bytes);
                REQUIRE_TRUE(snapshot.processed_bytes <= text.size());
                last = snapshot;
            });
            REQUIRE_EQ(last.total_bytes, text.size());
            REQUIRE_EQ(last.processed_bytes, text.size());
            REQUIRE_EQ(last.total_entries, 1U);
            REQUIRE_EQ(last.completed_entries, 1U);
            if (check == TextTransferCheck::BoundedCallbacks) {
                REQUIRE_TRUE(callbacks <= text.size() / 65536U + 6U);
            }
            REQUIRE_EQ(source_session_fixture_bytes(output / "payload.bin"), payload);
            std::filesystem::remove_all(output);
        } else if (check != TextTransferCheck::Readback) {
            require_text_transfer_interruption(archive, output, payload, extract, check, trailer_begin);
        }
    }
    std::filesystem::remove_all(cleanup.path);
}

}  // namespace flitzzip_test
