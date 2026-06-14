#include "core/archive.hpp"

#include "core/archive_index.hpp"
#include "core/checksum.hpp"
#include "core/file_manifest.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <span>
#include <vector>

namespace superzip {
namespace {

constexpr std::uint64_t kArchiveFooterSize = 24;
constexpr std::uint32_t kFooterMagic = 0x465A5355;  // USZF

// Purpose: Read a bounded chunk from an input file.
// Inputs: `input` is an open binary stream and `max_bytes` is the maximum byte count to allocate/read.
// Outputs: Returns the bytes read, possibly fewer at EOF; throws `ArchiveError` on read failure.
std::vector<std::byte> read_file_chunk(std::ifstream& input, std::uint64_t max_bytes) {
    std::vector<std::byte> buffer(static_cast<std::size_t>(max_bytes));
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto got = input.gcount();
    if (got < 0) {
        throw ArchiveError("failed to read input chunk");
    }
    buffer.resize(static_cast<std::size_t>(got));
    return buffer;
}

// Purpose: Query an output stream's current write position.
// Inputs: `stream` is an open archive output stream.
// Outputs: Returns the byte offset; throws `ArchiveError` when the offset cannot be queried.
std::uint64_t stream_position(std::ostream& stream) {
    const auto pos = stream.tellp();
    if (pos < 0) {
        throw ArchiveError("failed to query archive stream position");
    }
    return static_cast<std::uint64_t>(pos);
}

// Purpose: Query an input stream's current read position.
// Inputs: `stream` is an open archive input stream.
// Outputs: Returns the byte offset; throws `ArchiveError` when the offset cannot be queried.
std::uint64_t stream_position(std::istream& stream) {
    const auto pos = stream.tellg();
    if (pos < 0) {
        throw ArchiveError("failed to query archive stream position");
    }
    return static_cast<std::uint64_t>(pos);
}

// Purpose: Load the SuperZip index referenced by the archive footer.
// Inputs: `input` is an open binary archive stream.
// Outputs: Returns the parsed index with offset/size populated; throws `ArchiveError` for missing or invalid footer metadata.
ArchiveIndex read_index_from_file(std::ifstream& input) {
    input.seekg(0, std::ios::end);
    const auto size = stream_position(input);
    if (size < kArchiveFooterSize) {
        throw ArchiveError("archive is too small");
    }
    input.seekg(static_cast<std::streamoff>(size - kArchiveFooterSize), std::ios::beg);
    const auto footer_magic = read_u32(input);
    if (footer_magic != kFooterMagic) {
        throw ArchiveError("archive footer is missing");
    }
    const auto version = read_u32(input);
    if (version != kSuperZipVersion) {
        throw ArchiveError("unsupported archive footer version");
    }
    const auto index_offset = read_u64(input);
    const auto index_size = read_u64(input);
    if (index_offset > size || index_size > size || index_offset + index_size > size) {
        throw ArchiveError("archive index points outside file");
    }
    input.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    auto index = read_archive_index(input);
    index.index_offset = index_offset;
    index.index_size = index_size;
    return index;
}

// Purpose: Read a bounded payload range for one archive entry.
// Inputs: `input` is the archive stream, `entry` supplies payload base metadata, `archive_size` bounds the file, `relative_offset` is entry-relative payload offset, and `size` is bytes to read.
// Outputs: Returns payload bytes; throws `ArchiveError` when metadata points outside the archive or bytes are truncated.
std::vector<std::byte> read_payload_window(
    std::ifstream& input,
    const ArchiveEntry& entry,
    std::uint64_t archive_size,
    std::uint64_t relative_offset,
    std::uint64_t size) {
    if (entry.payload_offset > archive_size || entry.payload_size > archive_size ||
        entry.payload_offset > archive_size - entry.payload_size ||
        relative_offset > entry.payload_size || size > entry.payload_size - relative_offset) {
        throw ArchiveError("entry payload points outside archive");
    }
    input.seekg(static_cast<std::streamoff>(entry.payload_offset + relative_offset), std::ios::beg);
    std::vector<std::byte> payload(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (static_cast<std::uint64_t>(input.gcount()) != size) {
        throw ArchiveError("entry payload is truncated");
    }
    return payload;
}

// Purpose: Process one entry's block stream in bounded memory chunks.
// Inputs: `input` is the archive stream, `entry` is validated metadata, `archive_size` bounds reads, `options` controls chunk/GPU behavior, and `consume` receives decoded bytes.
// Outputs: Returns true if any chunk used AMD HIP; throws on malformed block windows, decode errors, or callback failures.
bool decode_entry_streaming(
    std::ifstream& input,
    const ArchiveEntry& entry,
    std::uint64_t archive_size,
    const ExtractOptions& options,
    const GpuCodecOptions& gpu_options,
    const std::function<void(std::span<const std::byte>)>& consume) {
    const auto chunk_limit = std::max<std::uint64_t>(options.chunk_size, options.block_size);
    bool gpu_used = false;
    std::size_t block_index = 0;
    while (block_index < entry.blocks.size()) {
        std::uint64_t uncompressed_window = 0;
        std::uint64_t payload_start = entry.payload_size;
        std::uint64_t payload_end = 0;
        const auto first = block_index;
        while (block_index < entry.blocks.size()) {
            const auto& block = entry.blocks[block_index];
            if (block.kind == BlockKind::Raw) {
                payload_start = std::min<std::uint64_t>(payload_start, block.encoded_offset);
                payload_end = std::max<std::uint64_t>(payload_end, block.encoded_offset + block.encoded_len);
            }
            uncompressed_window += block.uncompressed_len;
            ++block_index;
            if (uncompressed_window >= chunk_limit) {
                break;
            }
        }

        const bool has_payload = payload_end > payload_start;
        const auto payload_size = has_payload ? payload_end - payload_start : 0;
        auto payload = read_payload_window(input, entry, archive_size, has_payload ? payload_start : 0, payload_size);
        std::vector<BlockDescriptor> adjusted;
        adjusted.reserve(block_index - first);
        for (std::size_t i = first; i < block_index; ++i) {
            auto block = entry.blocks[i];
            if (block.kind == BlockKind::Raw) {
                block.encoded_offset -= payload_start;
            } else {
                block.encoded_offset = 0;
            }
            adjusted.push_back(block);
        }
        std::vector<std::byte> decoded(static_cast<std::size_t>(uncompressed_window));
        gpu_used = decode_chunk(payload, adjusted, decoded, gpu_options) || gpu_used;
        consume(std::span<const std::byte>(decoded.data(), decoded.size()));
    }
    return gpu_used;
}

// Purpose: Sum uncompressed block lengths for an entry.
// Inputs: `blocks` is the archive entry block table.
// Outputs: Returns total uncompressed bytes; does not allocate.
std::uint64_t sum_block_sizes(const std::vector<BlockDescriptor>& blocks) {
    std::uint64_t total = 0;
    for (const auto& block : blocks) {
        total += block.uncompressed_len;
    }
    return total;
}

// Purpose: Validate archive entry metadata before decoding or extraction.
// Inputs: `entry` is parsed archive metadata.
// Outputs: Returns normally when metadata is safe and consistent; throws `ArchiveError` or `SecurityError` otherwise.
void validate_entry_metadata(const ArchiveEntry& entry) {
    (void)safe_join_archive_path(std::filesystem::current_path(), entry.path);
    if (entry.directory) {
        if (entry.uncompressed_size != 0 || entry.payload_size != 0 || !entry.blocks.empty()) {
            throw ArchiveError("directory entry has payload metadata: " + entry.path);
        }
        return;
    }
    if (sum_block_sizes(entry.blocks) != entry.uncompressed_size) {
        throw ArchiveError("entry block sizes do not match uncompressed size: " + entry.path);
    }
    for (const auto& block : entry.blocks) {
        if (block.kind == BlockKind::Fill && block.encoded_len != 0) {
            throw ArchiveError("fill block contains payload bytes");
        }
        if (block.kind == BlockKind::Raw) {
            if (block.encoded_len != block.uncompressed_len ||
                block.encoded_len > entry.payload_size ||
                block.encoded_offset > entry.payload_size - block.encoded_len) {
                throw ArchiveError("raw block metadata is invalid");
            }
        }
    }
}

}  // namespace

OperationStats compress_szip(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const CompressOptions& options,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());

    std::ofstream output(output_archive, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ArchiveError("cannot create archive: " + output_archive.string());
    }

    ArchiveIndex index;
    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.entries = manifest.entries.size();
    const GpuCodecOptions gpu_options{
        .require_gpu = options.gpu_required,
        .block_size = options.block_size,
    };

    for (const auto& manifest_entry : manifest.entries) {
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        progress.set_current(manifest_entry.archive_path);
        publish_progress(progress, progress_callback);

        ArchiveEntry entry;
        entry.path = manifest_entry.archive_path;
        entry.directory = manifest_entry.directory;
        entry.uncompressed_size = manifest_entry.size;
        entry.payload_offset = stream_position(output);

        if (manifest_entry.directory) {
            index.entries.push_back(std::move(entry));
            progress.finish_entry();
            continue;
        }

        std::ifstream input(manifest_entry.source_path, std::ios::binary);
        if (!input) {
            throw ArchiveError("cannot open source file: " + manifest_entry.source_path.string());
        }

        std::uint64_t remaining = manifest_entry.size;
        std::uint64_t payload_written = 0;
        std::uint32_t crc = 0;
        while (remaining > 0) {
            const auto want = std::min<std::uint64_t>(remaining, options.chunk_size);
            auto chunk = read_file_chunk(input, want);
            if (chunk.empty() && want != 0) {
                throw ArchiveError("source file ended unexpectedly: " + manifest_entry.source_path.string());
            }
            crc = crc32(std::span<const std::byte>(chunk.data(), chunk.size()), crc);
            auto encoded = encode_chunk(chunk, gpu_options);
            stats.gpu_used = stats.gpu_used || encoded.gpu_used;
            for (auto block : encoded.blocks) {
                block.encoded_offset += payload_written;
                entry.blocks.push_back(block);
            }
            if (!encoded.payload.empty()) {
                output.write(
                    reinterpret_cast<const char*>(encoded.payload.data()),
                    static_cast<std::streamsize>(encoded.payload.size()));
                if (!output) {
                    throw ArchiveError("failed to write archive payload");
                }
                payload_written += encoded.payload.size();
            }
            remaining -= chunk.size();
            progress.add_bytes(chunk.size());
            publish_progress(progress, progress_callback);
        }
        entry.payload_size = payload_written;
        entry.crc32 = crc;
        index.entries.push_back(std::move(entry));
        progress.finish_entry();
    }

    index.index_offset = stream_position(output);
    write_archive_index(output, index);
    index.index_size = stream_position(output) - index.index_offset;
    write_u32(output, kFooterMagic);
    write_u32(output, kSuperZipVersion);
    write_u64(output, index.index_offset);
    write_u64(output, index.index_size);
    output.flush();
    if (!output) {
        throw ArchiveError("failed to finalize archive");
    }
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (options.verify_after_write) {
        (void)verify_szip(output_archive, ExtractOptions{.gpu_required = options.gpu_required, .block_size = options.block_size});
    }
    return stats;
}

OperationStats extract_szip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    const ExtractOptions& options,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open archive: " + archive_path.string());
    }
    const auto archive_size = std::filesystem::file_size(archive_path);
    auto index = read_index_from_file(input);

    std::uint64_t total_bytes = 0;
    for (const auto& entry : index.entries) {
        validate_entry_metadata(entry);
        total_bytes += entry.uncompressed_size;
    }
    std::filesystem::create_directories(destination);
    ProgressState progress;
    progress.start(OperationKind::Extract, total_bytes, index.entries.size());
    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = total_bytes;
    stats.entries = index.entries.size();

    const GpuCodecOptions gpu_options{
        .require_gpu = options.gpu_required,
        .block_size = options.block_size,
    };

    for (const auto& entry : index.entries) {
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
            progress.finish_entry();
            continue;
        }
        if (!options.overwrite && std::filesystem::exists(target)) {
            throw SecurityError("refusing to overwrite existing file: " + target.string());
        }
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create output file: " + target.string());
        }
        std::uint32_t actual_crc = 0;
        const bool decoded_on_gpu = decode_entry_streaming(
            input,
            entry,
            archive_size,
            options,
            gpu_options,
            [&](std::span<const std::byte> decoded) {
                actual_crc = crc32(decoded, actual_crc);
                output.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
                progress.add_bytes(decoded.size());
            });
        if (!output) {
            throw ArchiveError("failed to write output file: " + target.string());
        }
        if (actual_crc != entry.crc32) {
            throw ArchiveError("CRC mismatch while extracting: " + entry.path);
        }
        stats.gpu_used = stats.gpu_used || decoded_on_gpu;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats verify_szip(
    const std::filesystem::path& archive_path,
    const ExtractOptions& options,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open archive: " + archive_path.string());
    }
    const auto archive_size = std::filesystem::file_size(archive_path);
    auto index = read_index_from_file(input);
    std::uint64_t total_bytes = 0;
    for (const auto& entry : index.entries) {
        validate_entry_metadata(entry);
        total_bytes += entry.uncompressed_size;
    }
    ProgressState progress;
    progress.start(OperationKind::Verify, total_bytes, index.entries.size());
    const GpuCodecOptions gpu_options{
        .require_gpu = options.gpu_required,
        .block_size = options.block_size,
    };
    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = total_bytes;
    stats.entries = index.entries.size();
    for (const auto& entry : index.entries) {
        if (entry.directory) {
            progress.finish_entry();
            continue;
        }
        progress.set_current(entry.path);
        std::uint32_t actual_crc = 0;
        const bool decoded_on_gpu = decode_entry_streaming(
            input,
            entry,
            archive_size,
            options,
            gpu_options,
            [&](std::span<const std::byte> decoded) {
                actual_crc = crc32(decoded, actual_crc);
                progress.add_bytes(decoded.size());
            });
        if (actual_crc != entry.crc32) {
            throw ArchiveError("CRC mismatch while verifying: " + entry.path);
        }
        stats.gpu_used = stats.gpu_used || decoded_on_gpu;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
