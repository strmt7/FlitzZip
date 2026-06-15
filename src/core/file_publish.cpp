#include "core/file_publish.hpp"

#include "core/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace superzip {
namespace {

// Purpose: Return a short process-unique token for temporary output names.
// Inputs: None.
// Outputs: Returns a decimal process identifier on Windows or a thread-hash fallback elsewhere.
std::string current_process_token() {
#ifdef _WIN32
    return std::to_string(GetCurrentProcessId());
#else
    return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

#ifdef _WIN32
// Purpose: Classify Windows move errors that indicate the destination already exists.
// Inputs: `error` is the result from `GetLastError` after a failed `MoveFileExW`.
// Outputs: Returns true when the failure should be reported as an overwrite refusal.
bool is_existing_target_error(DWORD error) {
    return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
}
#endif

}  // namespace

// Purpose: Reserve a private temporary file target beside a final output file.
// Inputs: `target` is the final file path that will later receive verified bytes.
// Outputs: Returns a same-directory temporary directory and payload file path, or throws on reservation failure.
ReservedFilePublishTarget reserve_file_publish_target(const std::filesystem::path& target) {
    const auto process_token = current_process_token();
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto directory = target;
        directory += ".sztmp-";
        directory += process_token;
        directory += "-";
        directory += std::to_string(stamp);
        directory += "-";
        directory += std::to_string(attempt);

        std::error_code create_error;
        if (std::filesystem::create_directory(directory, create_error)) {
            return ReservedFilePublishTarget{
                .directory = directory,
                .file = directory / "payload.tmp",
            };
        }
        std::error_code exists_error;
        const bool directory_exists = std::filesystem::exists(directory, exists_error);
        if (create_error && !directory_exists) {
            throw ArchiveError("unable to reserve temporary output path: " + target.string());
        }
    }
    throw ArchiveError("unable to reserve temporary output path: " + target.string());
}

// Purpose: Remove a reserved temporary publication target after success or failure.
// Inputs: `temporary` is the reserved temporary directory and payload path.
// Outputs: Best-effort removal of only the known payload and its immediate temporary directory.
void cleanup_file_publish_target(const ReservedFilePublishTarget& temporary) {
    std::error_code ignored;
    std::filesystem::remove(temporary.file, ignored);
    std::filesystem::remove(temporary.directory, ignored);
}

// Purpose: Publish a fully verified temporary file into its final path.
// Inputs: `temporary` is the verified payload file, `target` is the final path, and `overwrite` controls replacement.
// Outputs: Atomically moves or links the file into place, or throws without deleting the caller-owned temporary file.
void commit_verified_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target,
    bool overwrite) {
#ifdef _WIN32
    auto flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(), flags) == 0) {
        const auto error = GetLastError();
        if (!overwrite && is_existing_target_error(error)) {
            throw SecurityError("refusing to overwrite existing file: " + target.string());
        }
        throw ArchiveError("failed to finalize extracted file: " + target.string());
    }
#else
    if (overwrite) {
        std::filesystem::rename(temporary, target);
        return;
    }

    std::error_code link_error;
    std::filesystem::create_hard_link(temporary, target, link_error);
    if (!link_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return;
    }

    std::error_code exists_error;
    if (std::filesystem::exists(target, exists_error)) {
        throw SecurityError("refusing to overwrite existing file: " + target.string());
    }
    throw ArchiveError("failed to finalize extracted file: " + target.string());
#endif
}

}  // namespace superzip
