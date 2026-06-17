#include "lzip/lzip_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"
#include "lzip/lzip_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace superzip {
namespace {

constexpr std::size_t kLzipAdapterBufferBytes = 64U * 1024U;

// Purpose: Add byte counts while detecting telemetry overflow.
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before unsigned wraparound.
void checked_add_lzip_adapter_bytes(std::uint64_t& total, std::uint64_t bytes, const char* context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    total += bytes;
}

// Purpose: Derive a safe single output entry name from the archive filename.
// Inputs: `archive_path` is the host path to the `.lz` stream.
// Outputs: Returns a relative archive entry name that can pass path-safety checks.
std::string lzip_output_entry_name(const std::filesystem::path& archive_path) {
    auto filename = archive_path.filename().string();
    auto lower = filename;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.size() > 7U && lower.ends_with(".tar.lz")) {
        filename.resize(filename.size() - 3U);
    } else if (lower.size() > 4U && lower.ends_with(".tlz")) {
        filename.resize(filename.size() - 4U);
        filename += ".tar";
    } else if (lower.size() > 3U && lower.ends_with(".lz")) {
        filename.resize(filename.size() - 3U);
    } else {
        filename = archive_path.stem().string();
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

}  // namespace

OperationStats extract_lzip_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto entry_name = lzip_output_entry_name(archive_path);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing lzip extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    LzipInputStream input(archive_path);
    ProgressState progress;
    progress.start(OperationKind::Extract, input.input_bytes(), 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create lzip extraction target: " + target.string());
        }

        std::array<char, kLzipAdapterBufferBytes> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                checked_add_lzip_adapter_bytes(output_size, static_cast<std::uint64_t>(bytes_read), "lzip output");
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write lzip extraction target: " + target.string());
                }
            }
        }
        input.finish();
        progress.add_bytes(input.input_bytes());
        publish_progress(progress, progress_callback);

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize lzip extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = input.input_bytes();
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
