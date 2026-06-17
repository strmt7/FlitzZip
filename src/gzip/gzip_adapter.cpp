#include "gzip/gzip_adapter.hpp"

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

#include "miniz.h"

namespace superzip {
namespace {

constexpr std::size_t kGzipBufferBytes = 64U * 1024U;
constexpr std::uint8_t kGzipFlagText = 0x01U;
constexpr std::uint8_t kGzipFlagHeaderCrc = 0x02U;
constexpr std::uint8_t kGzipFlagExtra = 0x04U;
constexpr std::uint8_t kGzipFlagName = 0x08U;
constexpr std::uint8_t kGzipFlagComment = 0x10U;
constexpr std::uint8_t kGzipReservedFlags = 0xE0U;

struct GzipHeader {
    std::uint64_t compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint32_t expected_isize = 0;
};

// Purpose: Convert an unsigned file offset to the signed type required by iostreams.
// Inputs: `value` is a byte offset and `context` identifies the caller for diagnostics.
// Outputs: Returns `value` as `std::streamoff`, or throws if the offset cannot be represented.
std::streamoff to_streamoff(std::uint64_t value, const char* context) {
    const auto max_streamoff = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (value > max_streamoff) {
        throw ArchiveError(std::string(context) + " offset exceeds host stream limits");
    }
    return static_cast<std::streamoff>(value);
}

// Purpose: Read a filesystem file size into the archive telemetry type.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried or represented.
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

// Purpose: Write all bytes in a span-like buffer to a binary stream.
// Inputs: `output` is the destination stream, `data` points to bytes, and `size` is the byte count.
// Outputs: Appends bytes or throws on stream failure.
void write_exact(std::ofstream& output, const unsigned char* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write Gzip stream");
    }
}

// Purpose: Write a little-endian 32-bit Gzip trailer field.
// Inputs: `output` is the destination stream and `value` is the CRC32 or ISIZE field.
// Outputs: Appends four bytes or throws on stream failure.
void write_le32(std::ofstream& output, std::uint32_t value) {
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value & 0xFFU),
        static_cast<unsigned char>((value >> 8U) & 0xFFU),
        static_cast<unsigned char>((value >> 16U) & 0xFFU),
        static_cast<unsigned char>((value >> 24U) & 0xFFU),
    };
    write_exact(output, bytes.data(), bytes.size());
}

// Purpose: Read a little-endian 32-bit Gzip trailer field.
// Inputs: `bytes` points to at least four bytes.
// Outputs: Returns the decoded unsigned field.
std::uint32_t read_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Purpose: Seek a stream to an absolute offset after bounds checking.
// Inputs: `input` is the source stream, `offset` is the target byte position, and `context` identifies the operation.
// Outputs: Positions the stream or throws on seek failure.
void seek_input(std::ifstream& input, std::uint64_t offset, const char* context) {
    input.seekg(to_streamoff(offset, context), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek Gzip ") + context);
    }
}

// Purpose: Advance over a bounded optional Gzip header field.
// Inputs: `input` is positioned by caller, `offset` is updated, `bytes` is the skip count, `limit` is the first
// compressed byte limit, and `field` names the field. Outputs: Advances `offset` and stream position, or throws when
// the field would overlap compressed data/trailer.
void skip_header_bytes(std::ifstream& input, std::uint64_t& offset, std::uint64_t bytes, std::uint64_t limit,
                       const char* field) {
    if (bytes > limit - offset) {
        throw ArchiveError(std::string("Gzip ") + field + " field exceeds header bounds");
    }
    offset += bytes;
    seek_input(input, offset, field);
}

// Purpose: Skip a bounded zero-terminated optional Gzip header string.
// Inputs: `input` is positioned at the field start, `offset` is updated, `limit` is the first compressed byte limit,
// and `field` names the field. Outputs: Positions the stream after the terminating NUL, or throws on
// unterminated/overlapping metadata.
void skip_zero_terminated_header_field(std::ifstream& input, std::uint64_t& offset, std::uint64_t limit,
                                       const char* field) {
    while (offset < limit) {
        char value = 0;
        input.read(&value, 1);
        if (!input) {
            throw ArchiveError(std::string("failed to read Gzip ") + field + " field");
        }
        ++offset;
        if (value == '\0') {
            return;
        }
    }
    throw ArchiveError(std::string("Gzip ") + field + " field is not terminated");
}

// Purpose: Parse Gzip wrapper metadata without trusting embedded names for output paths.
// Inputs: `input` is a binary Gzip stream and `file_size` is the total archive byte count.
// Outputs: Returns compressed payload bounds and trailer expectations, or throws on malformed wrapper data.
GzipHeader parse_gzip_header(std::ifstream& input, std::uint64_t file_size) {
    if (file_size < 18U) {
        throw ArchiveError("Gzip stream is too small");
    }
    const std::uint64_t compressed_limit = file_size - 8U;

    std::array<unsigned char, 10> header{};
    seek_input(input, 0, "header");
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input) {
        throw ArchiveError("failed to read Gzip header");
    }
    if (header[0] != 0x1FU || header[1] != 0x8BU || header[2] != 8U) {
        throw ArchiveError("invalid Gzip header");
    }
    const auto flags = header[3];
    if ((flags & kGzipReservedFlags) != 0U) {
        throw ArchiveError("Gzip header uses reserved flags");
    }
    // FTEXT is advisory only; extraction remains binary-safe regardless of it.

    std::uint64_t offset = header.size();
    if ((flags & kGzipFlagExtra) != 0U) {
        if (compressed_limit - offset < 2U) {
            throw ArchiveError("Gzip extra field length exceeds header bounds");
        }
        std::array<unsigned char, 2> extra_size{};
        input.read(reinterpret_cast<char*>(extra_size.data()), static_cast<std::streamsize>(extra_size.size()));
        if (!input) {
            throw ArchiveError("failed to read Gzip extra field length");
        }
        offset += extra_size.size();
        const auto bytes =
            static_cast<std::uint64_t>(extra_size[0]) | (static_cast<std::uint64_t>(extra_size[1]) << 8U);
        skip_header_bytes(input, offset, bytes, compressed_limit, "extra");
    }
    if ((flags & kGzipFlagName) != 0U) {
        skip_zero_terminated_header_field(input, offset, compressed_limit, "name");
    }
    if ((flags & kGzipFlagComment) != 0U) {
        skip_zero_terminated_header_field(input, offset, compressed_limit, "comment");
    }
    if ((flags & kGzipFlagHeaderCrc) != 0U) {
        skip_header_bytes(input, offset, 2U, compressed_limit, "header CRC");
    }
    if (offset >= compressed_limit) {
        throw ArchiveError("Gzip stream has no compressed payload");
    }

    std::array<unsigned char, 8> trailer{};
    seek_input(input, compressed_limit, "trailer");
    input.read(reinterpret_cast<char*>(trailer.data()), static_cast<std::streamsize>(trailer.size()));
    if (!input) {
        throw ArchiveError("failed to read Gzip trailer");
    }

    return GzipHeader{
        .compressed_offset = offset,
        .compressed_size = compressed_limit - offset,
        .expected_crc32 = read_le32(trailer.data()),
        .expected_isize = read_le32(trailer.data() + 4),
    };
}

// Purpose: Derive a safe single output entry name from the archive filename.
// Inputs: `archive_path` is the host path to the `.gz` stream.
// Outputs: Returns a relative archive entry name that can be passed through path safety checks.
std::string gzip_output_entry_name(const std::filesystem::path& archive_path) {
    auto filename = archive_path.filename().string();
    auto lower = filename;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.size() > 3U && lower.ends_with(".gz")) {
        filename.resize(filename.size() - 3U);
    } else {
        filename = archive_path.stem().string();
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

// Purpose: Write one regular file into a temporary Gzip archive and publish it atomically.
// Inputs: `source_file` is openable input, `output_archive` is the final target, `compression_level` is 1-9, `progress`
// is the caller-owned progress state, and `progress_callback` receives snapshots.
// Outputs: Publishes `output_archive` or throws after cleaning temporary compressor/output state.
void write_gzip_archive_payload(const std::filesystem::path& source_file, const std::filesystem::path& output_archive,
                                int compression_level, ProgressState& progress,
                                const ProgressCallback& progress_callback) {
    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Gzip source file: " + source_file.string());
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    mz_stream stream{};
    bool stream_active = false;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Gzip archive: " + output_archive.string());
        }

        const std::array<unsigned char, 10> header{0x1F, 0x8B, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 255U};
        write_exact(output, header.data(), header.size());
        if (mz_deflateInit2(&stream, compression_level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) !=
            MZ_OK) {
            throw ArchiveError("failed to initialize Gzip compressor");
        }
        stream_active = true;

        std::array<unsigned char, kGzipBufferBytes> input_buffer{};
        std::array<unsigned char, kGzipBufferBytes> output_buffer{};
        auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, nullptr, 0));
        std::uint64_t processed = 0;
        auto pump_deflate = [&](int flush) {
            int status = MZ_OK;
            do {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<unsigned int>(output_buffer.size());
                status = mz_deflate(&stream, flush);
                if (status != MZ_OK && status != MZ_STREAM_END) {
                    throw ArchiveError("Gzip compression failed");
                }
                const auto produced = output_buffer.size() - stream.avail_out;
                write_exact(output, output_buffer.data(), produced);
            } while (stream.avail_out == 0U || (flush == MZ_NO_FLUSH && stream.avail_in > 0U));
            return status;
        };

        for (;;) {
            input.read(reinterpret_cast<char*>(input_buffer.data()), static_cast<std::streamsize>(input_buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                crc = static_cast<std::uint32_t>(mz_crc32(crc, input_buffer.data(), bytes_read));
                checked_add_bytes(processed, bytes_read, "Gzip input");
                stream.next_in = input_buffer.data();
                stream.avail_in = static_cast<unsigned int>(bytes_read);
                while (stream.avail_in > 0U) {
                    (void)pump_deflate(MZ_NO_FLUSH);
                }
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad()) {
                throw ArchiveError("failed to read Gzip source file: " + source_file.string());
            }
            if (input.eof()) {
                break;
            }
        }

        int status = MZ_OK;
        do {
            status = pump_deflate(MZ_FINISH);
        } while (status != MZ_STREAM_END);
        mz_deflateEnd(&stream);
        stream_active = false;

        write_le32(output, crc);
        write_le32(output, static_cast<std::uint32_t>(processed & 0xFFFFFFFFULL));
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Gzip archive: " + output_archive.string());
        }

        commit_verified_file(temporary.file, output_archive, true);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (stream_active) {
            mz_deflateEnd(&stream);
        }
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
        throw ArchiveError("Gzip compression requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) && !equivalent_error) {
        throw SecurityError("refusing to overwrite the Gzip source file: " + output_archive.string());
    }

    const auto input_size = regular_file_size(source_file);
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(source_file.filename().string());
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

OperationStats extract_gzip_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                 bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Gzip archive: " + archive_path.string());
    }
    const auto header = parse_gzip_header(input, archive_size);
    const auto entry_name = gzip_output_entry_name(archive_path);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Gzip extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    // Progress is measured against the compressed payload because wrapper and
    // trailer bytes are parsed separately before extraction starts.
    ProgressState progress;
    progress.start(OperationKind::Extract, header.compressed_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    mz_stream stream{};
    bool stream_active = false;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Gzip extraction target: " + target.string());
        }
        if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
            throw ArchiveError("failed to initialize Gzip decompressor");
        }
        stream_active = true;
        seek_input(input, header.compressed_offset, "payload");

        std::array<unsigned char, kGzipBufferBytes> input_buffer{};
        std::array<unsigned char, kGzipBufferBytes> output_buffer{};
        auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, nullptr, 0));
        std::uint64_t output_size = 0;
        std::uint64_t remaining = header.compressed_size;
        int status = MZ_OK;

        // Inflate only the bounded payload range described by the parsed wrapper.
        // Any leftover deflate bytes before the trailer are treated as malformed.
        while (remaining > 0U && status != MZ_STREAM_END) {
            const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(input_buffer.size(), remaining));
            input.read(reinterpret_cast<char*>(input_buffer.data()), static_cast<std::streamsize>(to_read));
            if (static_cast<std::size_t>(input.gcount()) != to_read) {
                throw ArchiveError("Gzip compressed payload is truncated");
            }
            remaining -= to_read;
            stream.next_in = input_buffer.data();
            stream.avail_in = static_cast<unsigned int>(to_read);

            do {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<unsigned int>(output_buffer.size());
                status = mz_inflate(&stream, MZ_NO_FLUSH);
                if (status != MZ_OK && status != MZ_STREAM_END) {
                    throw ArchiveError("Gzip decompression failed");
                }
                const auto produced = output_buffer.size() - stream.avail_out;
                if (produced > 0U) {
                    crc = static_cast<std::uint32_t>(mz_crc32(crc, output_buffer.data(), produced));
                    checked_add_bytes(output_size, produced, "Gzip output");
                    write_exact(output, output_buffer.data(), produced);
                }
            } while ((stream.avail_out == 0U || stream.avail_in > 0U) && status != MZ_STREAM_END);

            progress.add_bytes(to_read);
            publish_progress(progress, progress_callback);
        }

        if (status != MZ_STREAM_END) {
            throw ArchiveError("Gzip compressed payload ended before the deflate stream completed");
        }
        if (stream.avail_in != 0U || remaining != 0U) {
            throw ArchiveError("Gzip stream contains trailing compressed data before the trailer");
        }
        mz_inflateEnd(&stream);
        stream_active = false;

        if (crc != header.expected_crc32) {
            throw ArchiveError("Gzip CRC32 verification failed");
        }
        if (static_cast<std::uint32_t>(output_size & 0xFFFFFFFFULL) != header.expected_isize) {
            throw ArchiveError("Gzip uncompressed-size verification failed");
        }
        // Publish only after CRC32 and ISIZE match the trailer.
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Gzip extraction target: " + target.string());
        }

        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
        progress.finish_entry();
        publish_progress(progress, progress_callback);

        OperationStats stats;
        stats.input_bytes = archive_size;
        stats.output_bytes = output_size;
        stats.entries = 1;
        stats.gpu_used = false;
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return stats;
    } catch (...) {
        if (stream_active) {
            mz_inflateEnd(&stream);
        }
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }
}

}  // namespace superzip
