#include "gzip/gzip_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/stream_archive_path.hpp"
#include "core/file_size.hpp"
#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

namespace superzip {
namespace {

constexpr std::size_t kGzipBufferBytes = 64U * 1024U;

// Purpose: Use the shared Gzip stream to compress one regular file before atomic publication.
// Inputs: `source_file` is openable input, `output_archive` is the final target, `compression_level` is 1-9, `progress`
// is the caller-owned progress state, and `progress_callback` receives snapshots.
// Outputs: Publishes `output_archive` or throws after cleaning temporary output; framing and compressor ownership
// remain shared with TAR.GZ and CPIO.GZ.
void write_gzip_archive_payload(const std::filesystem::path& source_file, const std::filesystem::path& output_archive,
                                int compression_level, ProgressState& progress,
                                const ProgressCallback& progress_callback) {
    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Gzip source file: " + path_diagnostic_utf8(source_file));
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    try {
        GzipOutputStream output(temporary.file, compression_level);
        std::array<char, kGzipBufferBytes> input_buffer{};
        for (;;) {
            input.read(input_buffer.data(), static_cast<std::streamsize>(input_buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                output.write(input_buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write Gzip stream");
                }
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad() || (input.fail() && !input.eof())) {
                throw ArchiveError("failed to read Gzip source file: " + path_diagnostic_utf8(source_file));
            }
            if (input.eof()) {
                break;
            }
        }

        output.close();

        commit_verified_file(temporary, output_archive, true);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }
}

}  // namespace

// Purpose: Create one `.gz` stream from one regular file with bounded raw-deflate compression.
// Inputs: `source_file` is the existing input, `output_archive` is the final target, `compression_level` is 1-9, and
// `progress_callback` receives snapshots. Outputs: Publishes a verified Gzip file and returns telemetry, or throws on
// invalid input, overwrite risk, or stream failure.
OperationStats compress_gzip_file(const std::filesystem::path& source_file, const std::filesystem::path& output_archive,
                                  int compression_level, const ProgressCallback& progress_callback) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Gzip compression level must be between 1 and 9");
    }
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Gzip compression requires one regular file: " + path_diagnostic_utf8(source_file));
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) && !equivalent_error) {
        throw SecurityError("refusing to overwrite the Gzip source file: " + path_diagnostic_utf8(output_archive));
    }

    const auto manifest = build_manifest({source_file});
    const auto& source_entry = manifest.entries.front();
    const auto source_lock = lock_manifest_source(source_entry);
    const auto input_size = source_entry.size;
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(path_diagnostic_utf8(source_file.filename()));
    publish_progress(progress, progress_callback);

    write_gzip_archive_payload(source_file, output_archive, compression_level, progress, progress_callback);

    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = input_size;
    stats.output_bytes = regular_file_size(output_archive);
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Create one `.gz` stream from an exactly one-item source list.
// Inputs: `sources` must contain one regular file, `output_archive` is the target, `compression_level` is 1-9, and
// `progress_callback` receives snapshots. Outputs: Returns compression telemetry or throws when the source contract or
// writer fails.
OperationStats compress_gzip(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& output_archive, int compression_level,
                             const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("Gzip compatibility requires exactly one regular-file source");
    }
    return compress_gzip_file(sources.front(), output_archive, compression_level, progress_callback);
}

// Purpose: Extract one Gzip member with verified framing, checksums, and UTF-8 output naming.
// Inputs: `archive_path`, `destination`, overwrite policy, and synchronous progress callback describe the operation.
// Outputs: Publishes the validated file and returns statistics, or throws without publishing incomplete output.
OperationStats extract_gzip_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                 bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    GzipInputStream input(archive_path);
    const auto entry_name = single_stream_entry_name(archive_path, {{".gz", ""}});
    create_verified_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name, ArchivePathEncoding::Utf8);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Gzip extraction target: " + path_diagnostic_utf8(target));
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, input.compressed_payload_bytes(), 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);
    FilePublishTransaction publication(target);
    std::ofstream output(publication.staging_path(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ArchiveError("cannot create Gzip extraction target: " + path_diagnostic_utf8(target));
    }
    std::array<char, kGzipBufferBytes> buffer{};
    std::uint64_t reported_payload_bytes = 0;
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if (bytes_read > 0) {
            output.write(buffer.data(), bytes_read);
            if (!output) {
                throw ArchiveError("failed to write Gzip extraction target: " + path_diagnostic_utf8(target));
            }
        }
        const auto payload_read = input.compressed_payload_read_bytes();
        progress.add_bytes(payload_read - reported_payload_bytes);
        reported_payload_bytes = payload_read;
        publish_progress(progress, progress_callback);
        if (input.bad()) {
            throw ArchiveError("failed to read Gzip stream");
        }
        if (input.eof()) {
            break;
        }
    }
    input.finish();
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize Gzip extraction target: " + path_diagnostic_utf8(target));
    }
    publication.commit(overwrite);
    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = input.input_bytes();
    stats.output_bytes = input.output_bytes();
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
