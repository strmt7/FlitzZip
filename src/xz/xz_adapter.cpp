#include "xz/xz_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/stream_archive_path.hpp"
#include "core/file_size.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/result.hpp"
#include "xz/xz_stream.hpp"

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

constexpr std::size_t kXzBufferBytes = 64U * 1024U;

}  // namespace

// Purpose: Decode one XZ compatibility stream into a verified output file.
// Inputs: `archive_path` is the encoded stream, `destination` is the extraction root, `overwrite` controls final-file
// replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Publishes the decoded file below `destination` and returns stats, or throws on malformed data or unsafe
// path.
OperationStats extract_xz_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                               bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    const auto entry_name = single_stream_entry_name(archive_path, {{".xz", ""}});
    create_verified_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name, ArchivePathEncoding::Utf8);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing XZ extraction target: " + path_diagnostic_utf8(target));
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        XzInputStream input(archive_path);
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create XZ extraction target: " + path_diagnostic_utf8(target));
        }

        std::array<char, kXzBufferBytes> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                output_size = checked_add_extracted_output_bytes(output_size, static_cast<std::uint64_t>(bytes_read),
                                                                 "XZ output");
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write XZ extraction target: " + path_diagnostic_utf8(target));
                }
            }
        }
        input.finish();
        progress.add_bytes(archive_size);
        publish_progress(progress, progress_callback);

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize XZ extraction target: " + path_diagnostic_utf8(target));
        }
        commit_verified_file(temporary, target, overwrite);
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
    stats.input_bytes = archive_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
