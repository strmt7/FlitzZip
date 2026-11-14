#include "rpm/rpm_adapter.hpp"
#include "core/path_text.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/archive_stream_progress.hpp"
#include "core/file_manifest.hpp"
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

namespace flitzzip {
namespace {

constexpr std::size_t kRpmIoBufferBytes = 256U * 1024U;
constexpr std::uint64_t kMaxRpmTemporaryPayloadBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

// Own the phase snapshot while borrowing the caller's original stateful observer
// only for this synchronous extraction. Preparation must not claim published bytes.
ProgressCheckpoint rpm_phase_checkpoint(const ProgressCallback& callback, const char* phase) {
    if (!callback) {
        return {};
    }
    return make_archive_phase_checkpoint([&callback](const ProgressSnapshot& snapshot) { callback(snapshot); },
                                         {.operation = OperationKind::Extract, .note = phase});
}

// Purpose: Enforce the maximum temporary RPM payload spool size.
// Inputs: `bytes` is the proposed compressed or decoded payload size and `label` names diagnostics.
// Outputs: Returns normally inside the cap or throws before disk usage can grow without bound.
void assert_rpm_temporary_payload_budget(std::uint64_t bytes, const char* label) {
    if (bytes > kMaxRpmTemporaryPayloadBytes) {
        throw ArchiveError(std::string(label) + " exceeds FlitzZip RPM temporary payload limit");
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
void copy_exact_bytes(std::ifstream& input, std::uint64_t remaining, std::ostream& output,
                      const ProgressCheckpoint& checkpoint) {
    std::array<char, kRpmIoBufferBytes> buffer{};
    while (remaining > 0U) {
        invoke_archive_checkpoint(checkpoint);
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
void copy_stream_to_file(std::istream& input, const std::filesystem::path& output_path,
                         const ProgressCheckpoint& checkpoint) {
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw ArchiveError("failed to create temporary RPM CPIO payload");
    }
    std::array<char, kRpmIoBufferBytes> buffer{};
    std::uint64_t written = 0;
    while (input) {
        invoke_archive_checkpoint(checkpoint);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto count_bytes = static_cast<std::uint64_t>(count);
            if (count_bytes > kMaxRpmTemporaryPayloadBytes - written) {
                throw ArchiveError("decoded RPM CPIO payload exceeds FlitzZip RPM temporary payload limit");
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
void copy_payload_tail(const std::filesystem::path& archive_path, const RpmPayloadInfo& payload,
                       const std::filesystem::path& target, const ProgressCheckpoint& checkpoint) {
    assert_rpm_temporary_payload_budget(payload.size, "compressed RPM payload");
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open RPM package: " + path_diagnostic_utf8(archive_path));
    }
    seek_payload_offset(input, payload.offset, "RPM payload");
    std::ofstream output(target, std::ios::binary);
    if (!output) {
        throw ArchiveError("failed to create temporary compressed RPM payload");
    }
    copy_exact_bytes(input, payload.size, output, checkpoint);
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize temporary compressed RPM payload");
    }
    invoke_archive_checkpoint(checkpoint);
}

// Preserve each decoder's own finish/checksum validation after closing the CPIO
// sink. Source, checkpoint and all temporary files remain owned by the caller.
template <class Reader>
void decode_rpm_payload(const std::filesystem::path& compressed, const std::filesystem::path& cpio,
                        const ProgressCheckpoint& checkpoint) {
    invoke_archive_checkpoint(checkpoint);
    Reader input(compressed);
    copy_stream_to_file(input, cpio, checkpoint);
    input.finish();
    invoke_archive_checkpoint(checkpoint);
}

// Purpose: Decode an RPM payload into a temporary CPIO file.
// Inputs: `archive_path` is the RPM file, `payload` describes the payload, `temporary_directory` holds intermediate
// files, and `cpio_path` receives decoded CPIO bytes. Outputs: Creates `cpio_path` or throws before extraction starts.
void materialize_cpio_payload(const std::filesystem::path& archive_path, const RpmPayloadInfo& payload,
                              const std::filesystem::path& temporary_directory, const std::filesystem::path& cpio_path,
                              const ProgressCheckpoint& preparing, const ProgressCheckpoint& decoding) {
    invoke_archive_checkpoint(preparing);
    if (payload.compression == RpmPayloadCompression::None) {
        assert_rpm_temporary_payload_budget(payload.size, "RPM CPIO payload");
        std::ifstream input(archive_path, std::ios::binary);
        if (!input) {
            throw ArchiveError("cannot open RPM package: " + path_diagnostic_utf8(archive_path));
        }
        seek_payload_offset(input, payload.offset, "RPM payload");
        std::ofstream output(cpio_path, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary RPM CPIO payload");
        }
        copy_exact_bytes(input, payload.size, output, preparing);
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary RPM CPIO payload");
        }
        invoke_archive_checkpoint(preparing);
        return;
    }

    const auto compressed_path = temporary_directory / "payload.compressed";
    copy_payload_tail(archive_path, payload, compressed_path, preparing);
    switch (payload.compression) {
    case RpmPayloadCompression::Gzip:
        return decode_rpm_payload<GzipInputStream>(compressed_path, cpio_path, decoding);
    case RpmPayloadCompression::Bzip2:
        return decode_rpm_payload<Bzip2InputStream>(compressed_path, cpio_path, decoding);
    case RpmPayloadCompression::Xz:
        return decode_rpm_payload<XzInputStream>(compressed_path, cpio_path, decoding);
    case RpmPayloadCompression::Zstd:
        return decode_rpm_payload<ZstdInputStream>(compressed_path, cpio_path, decoding);
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

// Purpose: Extract RPM using the UTF-8 default for its unmarked CPIO member names.
// Inputs: Archive, output root, overwrite policy, and synchronous progress callback.
// Outputs: Returns payload extraction statistics or propagates a decoding or publication error.
OperationStats extract_rpm(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
    return extract_rpm(archive_path, destination, overwrite, progress_callback, ArchivePathEncoding::Utf8);
}

// Purpose: Forward explicit filename encoding through RPM payload materialization to the CPIO reader.
// Inputs: Archive, output root, overwrite policy, synchronous progress callback, and explicit name encoding.
// Outputs: Returns extraction statistics and cleans temporary payloads, including on failure.
OperationStats extract_rpm(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback, ArchivePathEncoding encoding) {
    validate_archive_name_encoding(encoding);
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    const auto metadata_checkpoint = rpm_phase_checkpoint(progress_callback, "Reading RPM metadata");
    const auto preparing = rpm_phase_checkpoint(progress_callback, "Preparing RPM payload");
    const auto decoding = rpm_phase_checkpoint(progress_callback, "Decoding RPM payload");
    const auto payload = scan_rpm_payload(archive_source.path(), metadata_checkpoint);
    create_verified_directories(destination);

    const auto temporary = reserve_file_publish_target(destination / ".flitzzip-rpm-payload.cpio");
    try {
        materialize_cpio_payload(archive_source.path(), payload, temporary.directory, temporary.file, preparing,
                                 decoding);
        auto stats = extract_cpio(temporary.file, destination, overwrite, progress_callback, encoding);
        stats.input_bytes = archive_source.size();
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        cleanup_rpm_payload_target(temporary);
        return stats;
    } catch (...) {
        cleanup_rpm_payload_target(temporary);
        throw;
    }
}

}  // namespace flitzzip
