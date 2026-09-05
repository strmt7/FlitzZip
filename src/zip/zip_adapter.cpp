#include "zip/zip_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <chrono>
#include <limits>
#include <windows.h>

#include "miniz.h"

namespace superzip {
namespace {

// Purpose: Encode native filesystem paths for miniz's UTF-8 Windows stdio boundary.
// Inputs: An absolute or relative filesystem path, without archive-name normalization.
// Outputs: Returns UTF-8 path bytes or propagates an invalid native character conversion.
std::string zip_filesystem_path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Purpose: Decode ZIP's declared filename encoding before path validation and publication.
// Inputs: Validated miniz central-directory metadata with a bounded filename and general-purpose flags.
// Outputs: Returns UTF-8 metadata using EFS when set or the standard CP437 encoding otherwise.
std::string zip_entry_name(const mz_zip_archive_file_stat& entry) {
    constexpr mz_uint16 utf8_flag = 1U << 11U;
    const std::string name(entry.m_filename);
    if ((entry.m_bit_flag & utf8_flag) != 0U) {
        return name;
    }
    std::wstring decoded(name.size(), L'\0');
    const auto count = MultiByteToWideChar(437U, 0, name.data(), static_cast<int>(name.size()), decoded.data(),
                                           static_cast<int>(decoded.size()));
    if (count == 0) {
        throw ArchiveError("cannot decode ZIP CP437 filename");
    }
    decoded.resize(static_cast<std::size_t>(count));
    return zip_filesystem_path(std::filesystem::path(decoded));
}

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

    FilePublishTransaction publication(output_archive);
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_filesystem_path(publication.staging_path()).c_str(), 0)) {
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
            const auto source_lock = lock_manifest_source(entry);
            if (!mz_zip_writer_add_file(&zip, entry.archive_path.c_str(),
                                        zip_filesystem_path(entry.source_path).c_str(), nullptr, 0,
                                        static_cast<mz_uint>(compression_level))) {
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
        publication.commit(true);
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
    if (!mz_zip_reader_init_file(&zip, zip_filesystem_path(archive_path).c_str(),
                                 MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        throw ArchiveError("cannot open ZIP archive: " + archive_path.string());
    }
    try {
        const auto file_count = mz_zip_reader_get_num_files(&zip);
        if (file_count > kMaxArchiveEntries) {
            throw ArchiveError("ZIP entry count exceeds SuperZip resource limit");
        }
        std::uint64_t total_bytes = 0;
        std::uint64_t path_bytes = 0;
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
            auto name = zip_entry_name(stat);
            path_bytes =
                checked_add_archive_path_metadata_bytes(path_bytes, name.size(), "ZIP decoded filename metadata");
            path_entries.push_back(ArchivePathValidationEntry{
                .path = std::move(name),
                .directory = directory,
            });
        }
        validate_archive_path_set(path_entries);

        create_verified_directories(destination);
        ProgressState progress;
        progress.start(OperationKind::Extract, total_bytes, file_count);

        for (mz_uint i = 0; i < file_count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                throw ArchiveError("failed to read ZIP entry metadata");
            }
            const auto& name = path_entries[i].path;
            progress.set_current(name);
            publish_progress(progress, progress_callback);
            const auto target = safe_join_archive_path(destination, name, ArchivePathEncoding::Utf8);
            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                create_verified_directories(target);
                progress.finish_entry();
                continue;
            }
            if (!overwrite && std::filesystem::exists(target)) {
                throw SecurityError("refusing to overwrite existing ZIP extraction target: " + target.string());
            }
            // Miniz extracts to a private same-directory target first; only verified output is published.
            const auto temporary_target = reserve_file_publish_target(target);
            bool temporary_active = true;
            try {
                if (!mz_zip_reader_extract_to_file(&zip, i, zip_filesystem_path(temporary_target.file).c_str(), 0)) {
                    throw ArchiveError("failed to extract ZIP entry: " + std::string(stat.m_filename));
                }
                commit_verified_file(temporary_target, target, overwrite);
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
