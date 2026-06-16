#include "lha/lha_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "lha_input_stream.h"
#include "lha_reader.h"
}

namespace superzip {
namespace {

constexpr std::size_t kLhaReadBufferBytes = 256U * 1024U;
constexpr std::uint64_t kMaxLhaTotalFileBytes = kMaxPipelineMemoryBytes;

struct LhaEntry {
    std::string path;
    bool directory = false;
    std::uint64_t size = 0;
};

struct LhaMetadata {
    std::vector<LhaEntry> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Add an LHA file size to a bounded archive total.
// Inputs: `total` is the running byte count and `size` is the next file size.
// Outputs: Returns the updated total or throws before overflow/resource exhaustion.
std::uint64_t checked_add_lha_bytes(std::uint64_t total, std::uint64_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError("LHA uncompressed payload byte count overflows");
    }
    total += size;
    if (total > kMaxLhaTotalFileBytes) {
        throw ArchiveError("LHA uncompressed payload exceeds SuperZip resource limit");
    }
    return total;
}

// Purpose: Read bytes from a heap-owned C++ stream for the Lhasa C API.
// Inputs: `handle` points to an open `std::ifstream`, `buffer` receives bytes, and `buffer_len` is the requested byte count.
// Outputs: Returns bytes read or -1 when the stream reports a non-EOF read failure.
int lha_stream_read(void* handle, void* buffer, std::size_t buffer_len) {
    constexpr auto kMaxCallbackRead = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (handle == nullptr || buffer_len > kMaxCallbackRead) {
        return -1;
    }
    auto* input = static_cast<std::ifstream*>(handle);
    input->read(static_cast<char*>(buffer), static_cast<std::streamsize>(buffer_len));
    const auto read = input->gcount();
    if (read == static_cast<std::streamsize>(buffer_len) || input->eof()) {
        input->clear();
        return static_cast<int>(std::max<std::streamsize>(0, read));
    }
    return -1;
}

// Purpose: Skip forward in a heap-owned C++ stream for the Lhasa C API.
// Inputs: `handle` points to an open `std::ifstream` and `bytes` is the relative forward distance.
// Outputs: Returns non-zero when the seek succeeds.
int lha_stream_skip(void* handle, std::size_t bytes) {
    if (handle == nullptr || bytes > static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max())) {
        return 0;
    }
    auto* input = static_cast<std::ifstream*>(handle);
    input->clear();
    input->seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    return *input ? 1 : 0;
}

const LHAInputStreamType kSuperZipLhaInputStreamType{
    lha_stream_read,
    lha_stream_skip,
    nullptr,
};

class LhaReaderSession {
public:
    // Purpose: Open one LHA/LZH archive and bind it to a Lhasa reader.
    // Inputs: `archive_path` identifies the archive to decode.
    // Outputs: Owns the C stream/reader state or throws on open/allocation failure.
    explicit LhaReaderSession(const std::filesystem::path& archive_path) {
        input_ = std::make_unique<std::ifstream>(archive_path, std::ios::binary);
        if (!input_ || !*input_) {
            throw ArchiveError("cannot open LHA archive: " + archive_path.string());
        }
        stream_ = lha_input_stream_new(&kSuperZipLhaInputStreamType, input_.get());
        if (stream_ == nullptr) {
            throw ArchiveError("failed to initialize LHA input stream");
        }
        reader_ = lha_reader_new(stream_);
        if (reader_ == nullptr) {
            lha_input_stream_free(stream_);
            stream_ = nullptr;
            throw ArchiveError("failed to initialize LHA reader");
        }
    }

    LhaReaderSession(const LhaReaderSession&) = delete;
    LhaReaderSession& operator=(const LhaReaderSession&) = delete;

    // Purpose: Release Lhasa reader/stream resources.
    // Inputs: None.
    // Outputs: Frees C resources before closing the C++ stream.
    ~LhaReaderSession() {
        if (reader_ != nullptr) {
            lha_reader_free(reader_);
        }
        if (stream_ != nullptr) {
            lha_input_stream_free(stream_);
        }
    }

    // Purpose: Return the active C reader.
    // Inputs: None.
    // Outputs: Returns a non-null pointer while the session is alive.
    LHAReader* reader() const {
        return reader_;
    }

private:
    std::unique_ptr<std::ifstream> input_;
    LHAInputStream* stream_ = nullptr;
    LHAReader* reader_ = nullptr;
};

// Purpose: Return whether an LHA header represents a directory.
// Inputs: `header` is the current Lhasa header pointer.
// Outputs: Returns true for directory members and false for regular payloads.
bool lha_header_is_directory(const LHAFileHeader& header) {
    return std::strcmp(header.compress_method, LHA_COMPRESS_TYPE_DIR) == 0 &&
        header.symlink_target == nullptr;
}

// Purpose: Build the full archive path from Lhasa's split path/name fields.
// Inputs: `header` is untrusted archive metadata decoded by Lhasa.
// Outputs: Returns the raw archive path string for strict SuperZip normalization.
std::string lha_header_path(const LHAFileHeader& header) {
    if (header.symlink_target != nullptr) {
        throw SecurityError("LHA symbolic links are not supported");
    }

    std::string path = header.path == nullptr ? std::string{} : std::string(header.path);
    const std::string filename = header.filename == nullptr ? std::string{} : std::string(header.filename);
    if (filename.empty()) {
        return path;
    }
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(filename);
    return path;
}

// Purpose: Validate every LHA member path and payload before destination writes start.
// Inputs: `archive_path` identifies the archive to scan.
// Outputs: Returns safe metadata; throws on parser failure, unsafe path, unsupported entry type, CRC mismatch, or resource exhaustion.
LhaMetadata scan_lha_metadata(const std::filesystem::path& archive_path) {
    LhaReaderSession session(archive_path);
    LhaMetadata metadata;
    std::vector<ArchivePathValidationEntry> validation_entries;
    std::uint32_t entry_count = 0;

    for (;;) {
        LHAFileHeader* header = lha_reader_next_file(session.reader());
        if (header == nullptr) {
            break;
        }
        if (entry_count >= kMaxArchiveEntries) {
            throw ArchiveError("LHA entry count exceeds SuperZip resource limit");
        }
        ++entry_count;

        const bool directory = lha_header_is_directory(*header);
        const auto normalized_path = normalize_archive_path_key(lha_header_path(*header));
        if (!directory) {
            metadata.total_file_bytes = checked_add_lha_bytes(metadata.total_file_bytes, header->length);
            if (lha_reader_check(session.reader(), nullptr, nullptr) == 0) {
                throw ArchiveError("LHA payload CRC or size check failed: " + normalized_path);
            }
        }
        metadata.entries.push_back(LhaEntry{
            .path = normalized_path,
            .directory = directory,
            .size = directory ? 0U : header->length,
        });
        validation_entries.push_back(ArchivePathValidationEntry{
            .path = normalized_path,
            .directory = directory,
        });
    }

    if (metadata.entries.empty()) {
        throw ArchiveError("LHA archive contains no readable entries");
    }
    validate_archive_path_set(validation_entries);
    return metadata;
}

// Purpose: Write one decoded LHA payload through SuperZip's atomic publication path.
// Inputs: `reader` is positioned on `entry`, `destination` is the extraction root, and `overwrite` controls replacement.
// Outputs: Publishes the verified file or throws after cleanup.
void publish_lha_payload(
    LHAReader* reader,
    const LhaEntry& entry,
    const std::filesystem::path& destination,
    bool overwrite) {
    const auto target = safe_join_archive_path(destination, entry.path);
    std::filesystem::create_directories(target.parent_path());
    auto temporary = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary LHA extraction target: " + target.string());
        }

        std::array<unsigned char, kLhaReadBufferBytes> buffer{};
        std::uint64_t written = 0;
        while (written < entry.size) {
            const auto remaining = entry.size - written;
            const auto request = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            const auto decoded = lha_reader_read(reader, buffer.data(), request);
            if (decoded == 0) {
                throw ArchiveError("LHA decoder ended before expected payload size: " + entry.path);
            }
            if (decoded > request || decoded > std::numeric_limits<std::uint64_t>::max() - written) {
                throw ArchiveError("LHA decoder returned an invalid payload window: " + entry.path);
            }
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(decoded));
            if (!output) {
                throw ArchiveError("failed to write temporary LHA extraction target: " + target.string());
            }
            written += static_cast<std::uint64_t>(decoded);
        }

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary LHA extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
    } catch (...) {
        cleanup_file_publish_target(temporary);
        throw;
    }
}

// Purpose: Extract validated LHA entries to disk.
// Inputs: `archive_path` identifies the archive, `metadata` contains safe entries, `destination` is the extraction root, and `overwrite`/progress fields control publication behavior.
// Outputs: Writes directories and files below `destination`, or throws before leaving untracked temporary files.
void extract_lha_payloads(
    const std::filesystem::path& archive_path,
    const LhaMetadata& metadata,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    LhaReaderSession session(archive_path);
    ProgressState progress;
    progress.start(OperationKind::Extract, metadata.total_file_bytes, metadata.entries.size());
    std::size_t metadata_index = 0;

    for (;;) {
        LHAFileHeader* header = lha_reader_next_file(session.reader());
        if (header == nullptr) {
            break;
        }
        if (metadata_index >= metadata.entries.size()) {
            throw ArchiveError("LHA extraction pass produced more entries than the validation pass");
        }
        const auto& entry = metadata.entries[metadata_index++];
        const auto normalized_path = normalize_archive_path_key(lha_header_path(*header));
        if (normalized_path != entry.path || lha_header_is_directory(*header) != entry.directory) {
            throw ArchiveError("LHA extraction metadata changed between validation and write pass");
        }

        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        if (entry.directory) {
            std::filesystem::create_directories(safe_join_archive_path(destination, entry.path));
        } else {
            publish_lha_payload(session.reader(), entry, destination, overwrite);
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }

    if (metadata_index != metadata.entries.size()) {
        throw ArchiveError("LHA extraction pass ended before all validated entries were processed");
    }
}

}  // namespace

OperationStats extract_lha(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto metadata = scan_lha_metadata(archive_path);
    std::filesystem::create_directories(destination);
    extract_lha_payloads(archive_path, metadata, destination, overwrite, progress_callback);

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
