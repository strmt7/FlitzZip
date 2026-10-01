#include "zstd/zstd_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/stream_archive_path.hpp"
#include "core/file_size.hpp"
#include "core/result.hpp"
#include "zstd/zstd_stream.hpp"

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

constexpr std::size_t kZstdCopyBufferBytes = 64U * 1024U;

}  // namespace

// Purpose: Create one `.zst` stream from one regular file with bounded libzstd compression.
// Inputs: `sources` must contain one file, `output_archive` is the target, `compression_level` is 1-9, and
// `progress_callback` receives snapshots. Outputs: Publishes a verified Zstandard file and returns telemetry, or throws
// on invalid input, overwrite risk, or stream failure.
OperationStats compress_zstd(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& output_archive, int compression_level,
                             const ProgressCallback& progress_callback) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Zstandard compression level must be between 1 and 9");
    }
    if (sources.size() != 1U) {
        throw ArchiveError("Zstandard compatibility requires exactly one regular-file source");
    }
    const auto& source_file = sources.front();
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Zstandard compression requires one regular file: " + path_diagnostic_utf8(source_file));
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) && !equivalent_error) {
        throw SecurityError("refusing to overwrite the Zstandard source file: " + path_diagnostic_utf8(output_archive));
    }

    const auto manifest = build_manifest({source_file});
    const auto& source_entry = manifest.entries.front();
    const auto source_lock = lock_manifest_source(source_entry);
    const auto input_size = source_entry.size;
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(path_diagnostic_utf8(source_file.filename()));
    publish_progress(progress, progress_callback);

    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Zstandard source file: " + path_diagnostic_utf8(source_file));
    }
    FilePublishTransaction publication(output_archive);
    std::uint32_t compression_workers = 0;
    {
        ZstdOutputStream output(publication.staging_path(), compression_level, input_size);
        compression_workers = output.compression_workers();
        std::array<char, kZstdCopyBufferBytes> buffer{};
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write Zstandard stream");
                }
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad() || (input.fail() && !input.eof())) {
                throw ArchiveError("failed to read Zstandard source file: " + path_diagnostic_utf8(source_file));
            }
            if (input.eof()) {
                break;
            }
        }
        output.close();
        publication.commit(true);
    }

    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = input_size;
    stats.output_bytes = regular_file_size(output_archive);
    stats.workers = compression_workers;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Extract a single-file `.zst`/`.zstd` stream to a validated destination.
// Inputs: `archive_path`, `destination`, `overwrite`, and `progress_callback` define the extraction run.
// Outputs: Publishes the decoded file and returns telemetry, or throws on malformed streams or unsafe paths.
OperationStats extract_zstd_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                 bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    const auto entry_name = single_stream_entry_name(archive_path, {{".zstd", ""}, {".zst", ""}});
    create_verified_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name, ArchivePathEncoding::Utf8);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Zstandard extraction target: " +
                            path_diagnostic_utf8(target));
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    FilePublishTransaction publication(target);
    {
        ZstdInputStream input(archive_path);
        std::ofstream output(publication.staging_path(), std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Zstandard extraction target: " + path_diagnostic_utf8(target));
        }
        std::array<char, kZstdCopyBufferBytes> buffer{};
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write Zstandard extraction target: " + path_diagnostic_utf8(target));
                }
            }
            if (input.bad()) {
                throw ArchiveError("failed to read Zstandard stream");
            }
            if (input.eof()) {
                break;
            }
        }
        input.finish();
        progress.add_bytes(archive_size);
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Zstandard extraction target: " + path_diagnostic_utf8(target));
        }
        publication.commit(overwrite);
        progress.finish_entry();
        publish_progress(progress, progress_callback);

        OperationStats stats;
        stats.input_bytes = archive_size;
        stats.output_bytes = input.output_bytes();
        stats.entries = 1;
        stats.gpu_used = false;
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return stats;
    }
}

}  // namespace superzip
