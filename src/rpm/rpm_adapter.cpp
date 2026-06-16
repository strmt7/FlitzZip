#include "rpm/rpm_adapter.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/file_publish.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_stream.hpp"
#include "rpm/rpm_format.hpp"
#include "xz/xz_stream.hpp"
#include "zstd/zstd_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>

namespace superzip {
namespace {

constexpr std::size_t kRpmIoBufferBytes = 256U * 1024U;
constexpr std::uint64_t kMaxRpmTemporaryPayloadBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

// Purpose: Enforce the maximum temporary RPM payload spool size.
// Inputs: `bytes` is the proposed compressed or decoded payload size and `label` names diagnostics.
// Outputs: Returns normally inside the cap or throws before disk usage can grow without bound.
void assert_rpm_temporary_payload_budget(std::uint64_t bytes, const char* label) {
    if (bytes > kMaxRpmTemporaryPayloadBytes) {
        throw ArchiveError(std::string(label) + " exceeds SuperZip RPM temporary payload limit");
    }
}

// Purpose: Seek a file stream to a bounded RPM payload offset.
// Inputs: `input` is seekable, `offset` is the target byte offset, and `label` names diagnostics.
// Outputs: Positions the stream or throws when the seek cannot be represented/performed.
void seek_payload_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Copy an exact byte range from an RPM file to an output stream.
// Inputs: `input` is positioned at the first byte, `remaining` is the exact byte count, and `output` receives bytes.
// Outputs: Writes exactly `remaining` bytes or throws on truncation/write failure.
void copy_exact_bytes(std::ifstream& input, std::uint64_t remaining, std::ostream& output) {
    std::array<char, kRpmIoBufferBytes> buffer{};
    while (remaining > 0U) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError("RPM payload is truncated");
        }
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        if (!output) {
            throw ArchiveError("failed to write temporary RPM payload");
        }
        remaining -= chunk;
    }
}

// Purpose: Copy one complete stream into an output file.
// Inputs: `input` emits uncompressed CPIO bytes and `output_path` is the temporary CPIO file.
// Outputs: Writes the decoded CPIO stream or throws on read/write failure.
void copy_stream_to_file(std::istream& input, const std::filesystem::path& output_path) {
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw ArchiveError("failed to create temporary RPM CPIO payload");
    }
    std::array<char, kRpmIoBufferBytes> buffer{};
    std::uint64_t written = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto count_bytes = static_cast<std::uint64_t>(count);
            if (count_bytes > kMaxRpmTemporaryPayloadBytes - written) {
                throw ArchiveError("decoded RPM CPIO payload exceeds SuperZip RPM temporary payload limit");
            }
            written += count_bytes;
            output.write(buffer.data(), count);
            if (!output) {
                throw ArchiveError("failed to write temporary RPM CPIO payload");
            }
        }
    }
    if (!input.eof()) {
        throw ArchiveError("failed to decode RPM payload");
    }
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize temporary RPM CPIO payload");
    }
}

// Purpose: Copy the compressed RPM payload tail into a private temporary file.
// Inputs: `archive_path` is the RPM file, `payload` describes the tail, and `target` receives exact payload bytes.
// Outputs: Writes a compressed payload file suitable for the existing stream decoders.
void copy_payload_tail(
    const std::filesystem::path& archive_path,
    const RpmPayloadInfo& payload,
    const std::filesystem::path& target) {
    assert_rpm_temporary_payload_budget(payload.size, "compressed RPM payload");
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open RPM package: " + archive_path.string());
    }
    seek_payload_offset(input, payload.offset, "RPM payload");
    std::ofstream output(target, std::ios::binary);
    if (!output) {
        throw ArchiveError("failed to create temporary compressed RPM payload");
    }
    copy_exact_bytes(input, payload.size, output);
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize temporary compressed RPM payload");
    }
}

// Purpose: Decode an RPM payload into a temporary CPIO file.
// Inputs: `archive_path` is the RPM file, `payload` describes the payload, `temporary_directory` holds intermediate files, and `cpio_path` receives decoded CPIO bytes.
// Outputs: Creates `cpio_path` or throws before extraction starts.
void materialize_cpio_payload(
    const std::filesystem::path& archive_path,
    const RpmPayloadInfo& payload,
    const std::filesystem::path& temporary_directory,
    const std::filesystem::path& cpio_path) {
    if (payload.compression == RpmPayloadCompression::None) {
        assert_rpm_temporary_payload_budget(payload.size, "RPM CPIO payload");
        std::ifstream input(archive_path, std::ios::binary);
        if (!input) {
            throw ArchiveError("cannot open RPM package: " + archive_path.string());
        }
        seek_payload_offset(input, payload.offset, "RPM payload");
        std::ofstream output(cpio_path, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary RPM CPIO payload");
        }
        copy_exact_bytes(input, payload.size, output);
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary RPM CPIO payload");
        }
        return;
    }

    const auto compressed_path = temporary_directory / "payload.compressed";
    copy_payload_tail(archive_path, payload, compressed_path);
    switch (payload.compression) {
    case RpmPayloadCompression::Gzip: {
        GzipInputStream input(compressed_path);
        copy_stream_to_file(input, cpio_path);
        input.finish();
        return;
    }
    case RpmPayloadCompression::Bzip2: {
        Bzip2InputStream input(compressed_path);
        copy_stream_to_file(input, cpio_path);
        input.finish();
        return;
    }
    case RpmPayloadCompression::Xz: {
        XzInputStream input(compressed_path);
        copy_stream_to_file(input, cpio_path);
        input.finish();
        return;
    }
    case RpmPayloadCompression::Zstd: {
        ZstdInputStream input(compressed_path);
        copy_stream_to_file(input, cpio_path);
        input.finish();
        return;
    }
    case RpmPayloadCompression::None:
        return;
    }
}

// Purpose: Remove all RPM-specific temporary payload files.
// Inputs: `temporary` is the reserved payload target that owns a private directory.
// Outputs: Best-effort removal of the decoded and compressed temporary payload files and their directory.
void cleanup_rpm_payload_target(const ReservedFilePublishTarget& temporary) {
    std::error_code ignored;
    std::filesystem::remove(temporary.directory / "payload.compressed", ignored);
    cleanup_file_publish_target(temporary);
}

}  // namespace

OperationStats extract_rpm(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto payload = scan_rpm_payload(archive_path);
    std::filesystem::create_directories(destination);

    const auto temporary = reserve_file_publish_target(destination / ".superzip-rpm-payload.cpio");
    try {
        materialize_cpio_payload(archive_path, payload, temporary.directory, temporary.file);
        auto stats = extract_cpio(temporary.file, destination, overwrite, progress_callback);
        stats.input_bytes = std::filesystem::file_size(archive_path);
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        cleanup_rpm_payload_target(temporary);
        return stats;
    } catch (...) {
        cleanup_rpm_payload_target(temporary);
        throw;
    }
}

}  // namespace superzip
