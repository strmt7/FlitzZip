#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create an uncompressed POSIX TAR archive with SuperZip source-path validation.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination TAR path, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, or TAR writer failures.
OperationStats compress_tar(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Create a Gzip-compressed POSIX TAR archive with in-process streaming compression.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination `.tar.gz`/`.tgz`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, TAR writer, or Gzip writer failures.
OperationStats compress_tar_gzip(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Create a Bzip2-compressed POSIX TAR archive with in-process streaming compression.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination `.tar.bz2`/`.tbz2`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, TAR writer, or Bzip2 writer failures.
OperationStats compress_tar_bzip2(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Create a Zstandard-compressed POSIX TAR archive with in-process streaming compression.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination `.tar.zst`/`.tzst`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, TAR writer, or Zstandard writer failures.
OperationStats compress_tar_zstd(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract an uncompressed TAR archive with SuperZip path-safety checks.
// Inputs: `archive_path` is the TAR file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a Gzip-compressed POSIX TAR archive with two-pass stream validation.
// Inputs: `archive_path` is the `.tar.gz`/`.tgz` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed Gzip/TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar_gzip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a Bzip2-compressed POSIX TAR archive with two-pass stream validation.
// Inputs: `archive_path` is the `.tar.bz2`/`.tbz2` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed Bzip2/TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar_bzip2(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract an XZ-compressed POSIX TAR archive with two-pass stream validation.
// Inputs: `archive_path` is the `.tar.xz`/`.txz` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed XZ/TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar_xz(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a lzip-compressed POSIX TAR archive with two-pass stream validation.
// Inputs: `archive_path` is the `.tar.lz`/`.tlz` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed lzip/TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar_lzip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a Zstandard-compressed POSIX TAR archive with two-pass stream validation.
// Inputs: `archive_path` is the `.tar.zst`/`.tzst` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed Zstandard/TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar_zstd(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
