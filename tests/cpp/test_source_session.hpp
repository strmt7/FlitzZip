#pragma once

#include "test_util.hpp"

#include "core/archive.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <windows.h>

namespace flitzzip_test {

using ArchiveExtractor = flitzzip::OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&, bool,
                                                      const flitzzip::ProgressCallback&);

// Purpose: Inspect a small owned fixture without retaining a read handle during the actual sharing probe.
// Inputs: A regular test file no larger than one MiB. Outputs: Exact bytes, with the stream closed on return.
inline std::string source_session_fixture_bytes(const std::filesystem::path& path) {
    REQUIRE_TRUE(std::filesystem::file_size(path) <= 1024U * 1024U);
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Observe actual OS write admission without changing the archive bytes or pathname.
// Inputs: An owned source file; error receives the immediate failed-open status. Outputs: Availability; no live handle.
inline bool source_writer_available(const std::filesystem::path& path, DWORD& error) {
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    error = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (writer == INVALID_HANDLE_VALUE) {
        return false;
    }
    REQUIRE_TRUE(CloseHandle(writer) != FALSE);
    return true;
}

class SourceSessionInterrupted final : public std::runtime_error {
  public:
    // Purpose: Distinguish the caller's controlled cancellation from decoder or sharing errors.
    // Inputs: None. Outputs: A unique callback exception propagated across the public extraction operation.
    SourceSessionInterrupted() : std::runtime_error("controlled source-session callback interruption") {}
};

// Purpose: Preserve caller-owned destination bytes when Windows refuses replacement, then prove valid reuse.
// Inputs: A legal tiny one-file archive, an existing member and the actual public extractor; no source mutation.
// Outputs: Requires native publication refusal, exact caller bytes, no temporary debris, released locks and recovery.
inline void require_archive_destination_recovery(const std::filesystem::path& archive,
                                                 const std::filesystem::path& output, std::string_view member,
                                                 std::string_view expected, ArchiveExtractor extractor) {
    const auto target = output / member;
    const std::string sentinel = "caller-owned destination must survive refused publication";
    std::ofstream(target, std::ios::binary | std::ios::trunc) << sentinel;
    REQUIRE_EQ(source_session_fixture_bytes(target), sentinel);
    const auto entries_before = std::distance(std::filesystem::recursive_directory_iterator(output),
                                              std::filesystem::recursive_directory_iterator{});
    const auto reader = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(reader != INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> reader_owner(reader, &CloseHandle);
    unsigned callbacks = 0;
    bool refused = false;
    try {
        (void)extractor(archive, output, true, [&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::ArchiveError& error) {
        const std::string_view message(error.what());
        REQUIRE_TRUE(message.find("(Windows error 5)") != std::string_view::npos ||
                     message.find("(Windows error 32)") != std::string_view::npos);
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_TRUE(callbacks > 0U);
    REQUIRE_EQ(source_session_fixture_bytes(target), sentinel);
    REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(output),
                             std::filesystem::recursive_directory_iterator{}),
               entries_before);
    DWORD admission_error = ERROR_SUCCESS;
    REQUIRE_TRUE(source_writer_available(archive, admission_error));
    reader_owner.reset();
    REQUIRE_EQ(extractor(archive, output, true, {}).output_bytes, expected.size());
    REQUIRE_EQ(source_session_fixture_bytes(target), std::string(expected));
    REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(output),
                             std::filesystem::recursive_directory_iterator{}),
               entries_before);
}

// Purpose: Prove immutable source ownership, failed admission, exception cleanup and valid recovery at the public API.
// Inputs: A small valid archive, owned output root, member name, independent expected bytes and actual extractor.
// Outputs: Requires real write denial throughout extraction and unchanged source/output on failure; retains no handle.
inline void require_archive_source_session(const std::filesystem::path& archive, const std::filesystem::path& root,
                                           std::string_view member, std::string_view expected,
                                           ArchiveExtractor extractor) {
    const auto original = source_session_fixture_bytes(archive);
    const auto output = root / "source-session-output";
    unsigned callbacks = 0;
    bool writable_during_extraction = false;
    DWORD admission_error = ERROR_SUCCESS;
    const auto result = extractor(archive, output, false, [&](const flitzzip::ProgressSnapshot&) {
        ++callbacks;
        writable_during_extraction = source_writer_available(archive, admission_error) || writable_during_extraction;
    });
    REQUIRE_TRUE(callbacks > 0U);
    REQUIRE_TRUE(!writable_during_extraction);
    REQUIRE_EQ(admission_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(result.output_bytes, expected.size());
    REQUIRE_EQ(source_session_fixture_bytes(output / member), std::string(expected));
    REQUIRE_EQ(source_session_fixture_bytes(archive), original);
    REQUIRE_TRUE(source_writer_available(archive, admission_error));
    require_archive_destination_recovery(archive, output, member, expected, extractor);

    const auto writer =
        CreateFileW(archive.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(writer != INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> writer_owner(writer, &CloseHandle);
    const auto blocked_output = root / "source-session-blocked";
    bool refused = false;
    callbacks = 0;
    try {
        (void)extractor(archive, blocked_output, false, [&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(callbacks, 0U);
    REQUIRE_TRUE(!std::filesystem::exists(blocked_output));
    REQUIRE_EQ(source_session_fixture_bytes(output / member), std::string(expected));
    writer_owner.reset();
    REQUIRE_EQ(extractor(archive, blocked_output, false, {}).output_bytes, expected.size());
    REQUIRE_EQ(source_session_fixture_bytes(blocked_output / member), std::string(expected));

    bool interrupted = false;
    const auto interrupted_output = root / "source-session-interrupted";
    try {
        (void)extractor(archive, interrupted_output, false,
                        [](const flitzzip::ProgressSnapshot&) { throw SourceSessionInterrupted(); });
    } catch (const SourceSessionInterrupted&) {
        interrupted = true;
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_TRUE(!std::filesystem::exists(interrupted_output) || std::filesystem::is_empty(interrupted_output));
    REQUIRE_TRUE(source_writer_available(archive, admission_error));
    REQUIRE_EQ(source_session_fixture_bytes(archive), original);
    REQUIRE_EQ(extractor(archive, interrupted_output, false, {}).output_bytes, expected.size());
    REQUIRE_EQ(source_session_fixture_bytes(interrupted_output / member), std::string(expected));
}

// Purpose: Require an immutable source during read-only verification, including error cleanup and subsequent reuse.
// Inputs: One tiny owned archive, independent payload length and the actual verifier accepting a progress callback.
// Outputs: Exact input/output accounting, real write exclusion, typed refusal/cancellation and no directory changes.
template <typename Verify>
void require_archive_verification_source_session(const std::filesystem::path& archive, std::uint64_t payload_bytes,
                                                 Verify&& verify) {
    const auto original = source_session_fixture_bytes(archive);
    const auto entries_before = std::distance(std::filesystem::directory_iterator(archive.parent_path()),
                                              std::filesystem::directory_iterator{});
    DWORD admission_error = ERROR_SUCCESS;
    unsigned callbacks = 0;
    bool writable = false;
    const auto result = verify([&](const flitzzip::ProgressSnapshot&) {
        ++callbacks;
        writable = source_writer_available(archive, admission_error) || writable;
    });
    REQUIRE_TRUE(callbacks > 0U);
    REQUIRE_TRUE(!writable);
    REQUIRE_EQ(admission_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(result.input_bytes, original.size());
    REQUIRE_EQ(result.output_bytes, payload_bytes);
    REQUIRE_TRUE(source_writer_available(archive, admission_error));
    const auto writer =
        CreateFileW(archive.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(writer != INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> writer_owner(writer, &CloseHandle);
    callbacks = 0;
    bool rejected = false;
    try {
        (void)verify([&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(callbacks, 0U);
    writer_owner.reset();
    bool interrupted = false;
    try {
        (void)verify([](const flitzzip::ProgressSnapshot&) { throw SourceSessionInterrupted(); });
    } catch (const SourceSessionInterrupted&) {
        interrupted = true;
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_TRUE(source_writer_available(archive, admission_error));
    REQUIRE_EQ(verify({}).output_bytes, payload_bytes);
    REQUIRE_EQ(source_session_fixture_bytes(archive), original);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(archive.parent_path()),
                             std::filesystem::directory_iterator{}),
               entries_before);
}

}  // namespace flitzzip_test
