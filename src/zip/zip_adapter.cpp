#include "zip/zip_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <chrono>
#include <limits>

#include "miniz.h"

namespace superzip {
namespace {

// Purpose: Add ZIP metadata byte counters while enforcing SuperZip's extracted-output cap.
// Inputs: `lhs` and `rhs` are uncompressed byte counters from ZIP central directory metadata.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound or resource-limit excess.
std::uint64_t checked_add_zip_bytes(std::uint64_t lhs, std::uint64_t rhs) {
    return checked_add_extracted_output_bytes(lhs, rhs, "ZIP uncompressed payload");
}

}  // namespace

// Purpose: Create a standard ZIP archive from one or more source paths.
// Inputs: `sources`, `output_archive`, `compression_level`, and optional `progress_callback` describe the compatibility
// archive run. Outputs: Writes a ZIP archive and returns operation telemetry, or throws on source/read/write failure.
OperationStats compress_zip(const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& output_archive, int compression_level,
                            const ProgressCallback& progress_callback) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("ZIP compression level must be between 1 and 9");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, output_archive.string().c_str(), 0)) {
        throw ArchiveError("cannot create ZIP archive: " + output_archive.string());
    }
    bool finalized = false;
    try {
        for (const auto& entry : manifest.entries) {
            progress.set_current(entry.archive_path);
            publish_progress(progress, progress_callback);
            if (entry.directory) {
                const auto name = entry.archive_path.ends_with('/') ? entry.archive_path : entry.archive_path + "/";
                if (!mz_zip_writer_add_mem(&zip, name.c_str(), nullptr, 0, MZ_NO_COMPRESSION)) {
                    throw ArchiveError("failed to add ZIP directory: " + entry.archive_path);
                }
                progress.finish_entry();
                continue;
            }
            if (!mz_zip_writer_add_file(&zip, entry.archive_path.c_str(), entry.source_path.string().c_str(), nullptr,
                                        0, static_cast<mz_uint>(compression_level))) {
                throw ArchiveError("failed to add ZIP file: " + entry.archive_path);
            }
            progress.add_bytes(entry.size);
            progress.finish_entry();
        }
        if (!mz_zip_writer_finalize_archive(&zip)) {
            throw ArchiveError("failed to finalize ZIP archive");
        }
        finalized = true;
        mz_zip_writer_end(&zip);
    } catch (...) {
        if (!finalized) {
            mz_zip_writer_end(&zip);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.entries = manifest.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Extract a standard ZIP archive with SuperZip path safety and verified final-file publication.
// Inputs: `archive_path`, `destination`, `overwrite`, and optional `progress_callback` describe the extraction run.
// Outputs: Restores verified ZIP entries into `destination` and returns operation telemetry, or throws on validation
// failure.
OperationStats extract_zip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive_path.string().c_str(), 0)) {
        throw ArchiveError("cannot open ZIP archive: " + archive_path.string());
    }
    try {
        const auto file_count = mz_zip_reader_get_num_files(&zip);
        if (file_count > kMaxArchiveEntries) {
            throw ArchiveError("ZIP entry count exceeds SuperZip resource limit");
        }
        std::uint64_t total_bytes = 0;
        std::vector<ArchivePathValidationEntry> path_entries;
        path_entries.reserve(file_count);
        // Validate every entry path before any filesystem output is created.
        for (mz_uint i = 0; i < file_count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                throw ArchiveError("failed to read ZIP entry metadata");
            }
            const bool directory = mz_zip_reader_is_file_a_directory(&zip, i) != 0;
            if (!directory) {
                total_bytes = checked_add_zip_bytes(total_bytes, stat.m_uncomp_size);
            }
            path_entries.push_back(ArchivePathValidationEntry{
                .path = stat.m_filename,
                .directory = directory,
            });
        }
        validate_archive_path_set(path_entries);

        std::filesystem::create_directories(destination);
        ProgressState progress;
        progress.start(OperationKind::Extract, total_bytes, file_count);

        for (mz_uint i = 0; i < file_count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                throw ArchiveError("failed to read ZIP entry metadata");
            }
            progress.set_current(stat.m_filename);
            publish_progress(progress, progress_callback);
            const auto target = safe_join_archive_path(destination, stat.m_filename);
            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                std::filesystem::create_directories(target);
                progress.finish_entry();
                continue;
            }
            if (!overwrite && std::filesystem::exists(target)) {
                throw SecurityError("refusing to overwrite existing ZIP extraction target: " + target.string());
            }
            std::filesystem::create_directories(target.parent_path());
            // Miniz extracts to a private same-directory target first; only verified output is published.
            const auto temporary_target = reserve_file_publish_target(target);
            bool temporary_active = true;
            try {
                if (!mz_zip_reader_extract_to_file(&zip, i, temporary_target.file.string().c_str(), 0)) {
                    throw ArchiveError("failed to extract ZIP entry: " + std::string(stat.m_filename));
                }
                commit_verified_file(temporary_target.file, target, overwrite);
                cleanup_file_publish_target(temporary_target);
                temporary_active = false;
            } catch (...) {
                if (temporary_active) {
                    // Remove only SuperZip's known temporary payload and its private directory.
                    cleanup_file_publish_target(temporary_target);
                }
                throw;
            }
            progress.add_bytes(stat.m_uncomp_size);
            progress.finish_entry();
        }
        mz_zip_reader_end(&zip);

        OperationStats stats;
        stats.input_bytes = std::filesystem::file_size(archive_path);
        stats.output_bytes = total_bytes;
        stats.entries = file_count;
        stats.gpu_used = false;
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return stats;
    } catch (...) {
        mz_zip_reader_end(&zip);
        throw;
    }
}

}  // namespace superzip
