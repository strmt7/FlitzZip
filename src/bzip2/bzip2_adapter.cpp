#include "bzip2/bzip2_adapter.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace superzip {
namespace {

constexpr std::size_t kBzip2BufferBytes = 64U * 1024U;

// Purpose: Read a filesystem file size into the archive telemetry type.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
std::uint64_t regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// Purpose: Add byte counts while detecting telemetry overflow.
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before unsigned wraparound.
void checked_add_bytes(std::uint64_t& total, std::uint64_t bytes, const char* context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    total += bytes;
}

// Purpose: Derive a safe single output entry name from the archive filename.
// Inputs: `archive_path` is the host path to the `.bz2` stream.
// Outputs: Returns a relative archive entry name that can pass path-safety checks.
std::string bzip2_output_entry_name(const std::filesystem::path& archive_path) {
    auto filename = archive_path.filename().string();
    auto lower = filename;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.size() > 4U && lower.ends_with(".bz2")) {
        filename.resize(filename.size() - 4U);
    } else {
        filename = archive_path.stem().string();
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

}  // namespace

// Purpose: Create one `.bz2` stream from one regular file with bounded libbzip2 compression.
// Inputs: `source_file` is the existing input, `output_archive` is the target, `compression_level` is 1-9, and
// `progress_callback` receives snapshots. Outputs: Publishes a verified Bzip2 file and returns telemetry, or throws on
// invalid input, overwrite risk, or stream failure.
OperationStats compress_bzip2_file(const std::filesystem::path& source_file,
                                   const std::filesystem::path& output_archive, int compression_level,
                                   const ProgressCallback& progress_callback) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Bzip2 compression level must be between 1 and 9");
    }
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Bzip2 compression requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) && !equivalent_error) {
        throw SecurityError("refusing to overwrite the Bzip2 source file: " + output_archive.string());
    }

    const auto input_size = regular_file_size(source_file);
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(source_file.filename().string());
    publish_progress(progress, progress_callback);

    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Bzip2 source file: " + source_file.string());
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        Bzip2OutputStream output(temporary.file, compression_level);
        std::array<char, kBzip2BufferBytes> buffer{};
        for (;;) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write Bzip2 archive: " + output_archive.string());
                }
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad()) {
                throw ArchiveError("failed to read Bzip2 source file: " + source_file.string());
            }
            if (input.eof()) {
                break;
            }
        }
        output.close();
        output_size = output.output_bytes();
        commit_verified_file(temporary.file, output_archive, true);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }

    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = input_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Create one `.bz2` stream from an exactly one-item source list.
// Inputs: `sources` must contain one regular file, `output_archive` is the target, `compression_level` is 1-9, and
// `progress_callback` receives snapshots. Outputs: Returns compression telemetry or throws when the source contract or
// writer fails.
OperationStats compress_bzip2(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& output_archive, int compression_level,
                              const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("Bzip2 compatibility requires exactly one regular-file source");
    }
    return compress_bzip2_file(sources.front(), output_archive, compression_level, progress_callback);
}

OperationStats extract_bzip2_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                  bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    const auto entry_name = bzip2_output_entry_name(archive_path);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Bzip2 extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        Bzip2InputStream input(archive_path);
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Bzip2 extraction target: " + target.string());
        }

        std::array<char, kBzip2BufferBytes> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                checked_add_bytes(output_size, static_cast<std::uint64_t>(bytes_read), "Bzip2 output");
                output.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
                if (!output) {
                    throw ArchiveError("failed to write Bzip2 extraction target: " + target.string());
                }
            }
        }
        input.finish();
        progress.add_bytes(archive_size);
        publish_progress(progress, progress_callback);

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Bzip2 extraction target: " + target.string());
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
    stats.input_bytes = archive_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
