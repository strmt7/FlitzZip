#include "zip/zip_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/path_text.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <chrono>
#include <exception>
#include <limits>
#include <windows.h>

#include "miniz.h"

namespace superzip {
namespace {

// Purpose: Encode a native file path for miniz's UTF-8 Windows stdio boundary.
// Inputs: `path` identifies a filesystem object and may exceed the legacy Windows path limit.
// Outputs: Returns an absolute, extended-length UTF-8 path with native separators.
std::string zip_filesystem_path(const std::filesystem::path& path) {
    const auto text = std::filesystem::path(windows_api_path(path)).u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Purpose: Decode ZIP's declared filename encoding before path validation and publication.
// Inputs: The full, bounded central-directory filename and its general-purpose flags.
// Outputs: Returns UTF-8 metadata using EFS when set or the standard CP437 encoding otherwise.
std::string zip_entry_name(const std::string& name, mz_uint16 flags) {
    constexpr mz_uint16 utf8_flag = 1U << 11U;
    const bool utf8 = (flags & utf8_flag) != 0U;
    std::wstring decoded(name.size(), L'\0');
    const auto count =
        MultiByteToWideChar(utf8 ? CP_UTF8 : 437U, utf8 ? MB_ERR_INVALID_CHARS : 0U, name.data(),
                            static_cast<int>(name.size()), decoded.data(), static_cast<int>(decoded.size()));
    if (count == 0) {
        throw ArchiveError("cannot decode ZIP filename in its declared encoding");
    }
    decoded.resize(static_cast<std::size_t>(count));
    return path_diagnostic_utf8(std::filesystem::path(decoded));
}

// Purpose: Add ZIP metadata byte counters while enforcing SuperZip's extracted-output cap.
// Inputs: `lhs` and `rhs` are uncompressed byte counters from ZIP central directory metadata.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound or resource-limit excess.
std::uint64_t checked_add_zip_bytes(std::uint64_t lhs, std::uint64_t rhs) {
    return checked_add_extracted_output_bytes(lhs, rhs, "ZIP uncompressed payload");
}

struct ZipScanResult {
    std::vector<ArchivePathValidationEntry> paths;
    std::uint64_t total_bytes = 0;
};

// Purpose: Read complete ZIP member names without the diagnostic stat structure's 511-byte truncation.
// Inputs: `zip` is an initialized reader and `index` selects an entry.
// Outputs: Returns raw filename bytes or throws before accepting empty, embedded-NUL, or over-budget names.
std::string zip_full_entry_name(mz_zip_archive& zip, mz_uint index) {
    const auto size = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (size <= 1U || size - 1U > kMaxArchivePathBytes) {
        throw ArchiveError("ZIP filename is empty or exceeds SuperZip resource limits");
    }
    std::string name(size, '\0');
    if (mz_zip_reader_get_filename(&zip, index, name.data(), size) != size) {
        throw ArchiveError("failed to read complete ZIP filename");
    }
    name.pop_back();
    if (name.find('\0') != std::string::npos) {
        throw SecurityError("ZIP filename contains an embedded NUL");
    }
    return name;
}

// Purpose: Apply one metadata admission policy to ZIP extraction and read-only verification.
// Inputs: `zip` is an initialized reader containing untrusted central-directory entries.
// Outputs: Returns bounded decoded paths and byte totals, or throws before output on unsupported or unsafe metadata.
ZipScanResult scan_zip_entries(mz_zip_archive& zip) {
    const auto file_count = mz_zip_reader_get_num_files(&zip);
    if (file_count > kMaxArchiveEntries) {
        throw ArchiveError("ZIP entry count exceeds SuperZip resource limit");
    }
    ZipScanResult result;
    std::uint64_t path_bytes = 0;
    result.paths.reserve(file_count);
    for (mz_uint i = 0; i < file_count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            throw ArchiveError("failed to read ZIP entry metadata");
        }
        if (!stat.m_is_supported) {
            throw ArchiveError("ZIP entry uses an unsupported method or encryption");
        }
        if (!stat.m_is_directory) {
            result.total_bytes = checked_add_zip_bytes(result.total_bytes, stat.m_uncomp_size);
        }
        auto name = zip_entry_name(zip_full_entry_name(zip, i), stat.m_bit_flag);
        path_bytes = checked_add_archive_path_metadata_bytes(path_bytes, name.size(), "ZIP decoded filename metadata");
        result.paths.push_back({.path = std::move(name), .directory = stat.m_is_directory != 0});
    }
    validate_archive_path_set(result.paths);
    return result;
}

struct ZipVerificationOutput {
    ProgressState& progress;
    const ProgressCallback& callback;
    std::uint64_t expected_bytes = 0;
    std::uint64_t decoded_bytes = 0;
    std::exception_ptr failure;
};

// Purpose: Count bounded decoder output and propagate progress without unwinding through the C library.
// Inputs: `opaque` owns synchronous verification state; `offset` and `size` describe decoded bytes being discarded.
// Outputs: Returns the accepted size, or latches an exception and returns zero for the C caller to stop cleanly.
std::size_t consume_zip_verification(void* opaque, mz_uint64 offset, const void*, std::size_t size) noexcept {
    auto& output = *static_cast<ZipVerificationOutput*>(opaque);
    if (output.failure) {
        return 0;
    }
    try {
        if (offset != output.decoded_bytes || offset > output.expected_bytes || size > output.expected_bytes - offset) {
            throw ArchiveError("ZIP decoded payload exceeds its declared size");
        }
        output.decoded_bytes += size;
        output.progress.add_bytes(size);
        publish_progress(output.progress, output.callback);
        return size;
    } catch (...) {
        output.failure = std::current_exception();
        return 0;
    }
}

}  // namespace

// Purpose: Create a standard ZIP archive from one or more source paths.
// Inputs: `sources`, `output_archive`, `compression_level`, and `progress_callback` describe the run;
// `verify_after_write` enables read-back of private output before publication.
// Outputs: Publishes a ZIP and returns telemetry, or preserves the destination on prepublication failure/cancellation.
OperationStats compress_zip(const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& output_archive, int compression_level,
                            const ProgressCallback& progress_callback, bool verify_after_write) {
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
    std::uint64_t output_bytes = 0;
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
        if (!mz_zip_writer_end(&zip)) {
            throw ArchiveError("failed to close ZIP archive");
        }
        if (verify_after_write) {
            static_cast<void>(verify_zip(publication.staging_path(), progress_callback));
        }
        output_bytes = std::filesystem::file_size(publication.staging_path());
        publication.commit(true);
    } catch (...) {
        if (!finalized) {
            mz_zip_writer_end(&zip);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.output_bytes = output_bytes;
    stats.entries = manifest.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Validate ZIP metadata and decoded CRC/size without creating extraction files.
// Inputs: `archive_path` identifies the ZIP; `progress_callback` may throw to cancel at bounded decode checkpoints.
// Outputs: Returns verification telemetry or propagates a format, resource, path, decoder, or callback failure.
OperationStats verify_zip(const std::filesystem::path& archive_path, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_filesystem_path(archive_path).c_str(),
                                 MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        throw ArchiveError("cannot open ZIP archive: " + archive_path.string());
    }
    try {
        const auto scan = scan_zip_entries(zip);
        ProgressState progress;
        progress.start(OperationKind::Verify, scan.total_bytes, scan.paths.size());
        publish_progress(progress, progress_callback);
        for (mz_uint i = 0; i < scan.paths.size(); ++i) {
            const auto& entry = scan.paths[i];
            progress.set_current(entry.path);
            publish_progress(progress, progress_callback);
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat) ||
                !mz_zip_validate_file(&zip, i, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)) {
                throw ArchiveError("ZIP entry headers failed verification: " + entry.path);
            }
            ZipVerificationOutput output{
                .progress = progress, .callback = progress_callback, .expected_bytes = stat.m_uncomp_size};
            const bool decoded = mz_zip_reader_extract_to_callback(&zip, i, consume_zip_verification, &output, 0) != 0;
            if (output.failure) {
                std::rethrow_exception(output.failure);
            }
            if (!decoded || output.decoded_bytes != stat.m_uncomp_size ||
                (stat.m_uncomp_size == 0 && stat.m_crc32 != 0)) {
                throw ArchiveError("ZIP entry payload failed verification: " + entry.path);
            }
            progress.finish_entry();
            publish_progress(progress, progress_callback);
        }
        OperationStats stats;
        stats.input_bytes = std::filesystem::file_size(archive_path);
        stats.output_bytes = scan.total_bytes;
        stats.entries = scan.paths.size();
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        mz_zip_reader_end(&zip);
        return stats;
    } catch (...) {
        mz_zip_reader_end(&zip);
        throw;
    }
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
        const auto scan = scan_zip_entries(zip);
        const auto file_count = mz_zip_reader_get_num_files(&zip);

        create_verified_directories(destination);
        ProgressState progress;
        progress.start(OperationKind::Extract, scan.total_bytes, file_count);

        for (mz_uint i = 0; i < file_count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                throw ArchiveError("failed to read ZIP entry metadata");
            }
            const auto& name = scan.paths[i].path;
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
        stats.output_bytes = scan.total_bytes;
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
