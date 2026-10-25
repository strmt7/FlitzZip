#include "lha/lha_adapter.hpp"
#include "core/path_text.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limit_checks.hpp"
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

namespace flitzzip {
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
    std::uint64_t path_metadata_bytes = 0;
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
        throw ArchiveError("LHA uncompressed payload exceeds FlitzZip resource limit");
    }
    return total;
}

struct LhaInputContext {
    std::ifstream input;
    std::uint64_t size = 0;
    std::uint64_t delivered = 0;
    bool failed = false;
};

// Purpose: Distinguish verified physical EOF from an unreadable extent across the Lhasa C boundary.
// Inputs: `handle` borrows a session-owned context bound to a live pinned source; `buffer_len` bounds `buffer`.
// Outputs: Returns complete bounded reads, zero at the sealed extent or for an empty request, or sticky -1; never
// throws.
int lha_stream_read(void* handle, void* buffer, std::size_t buffer_len) noexcept {
    constexpr auto kMaxCallbackRead = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (handle == nullptr) {
        return -1;
    }
    auto& context = *static_cast<LhaInputContext*>(handle);
    if (context.failed || context.delivered > context.size || buffer_len > kMaxCallbackRead ||
        (buffer == nullptr && buffer_len != 0)) {
        context.failed = true;
        return -1;
    }
    const auto request =
        static_cast<std::streamsize>(std::min<std::uint64_t>(buffer_len, context.size - context.delivered));
    if (request == 0) {
        return 0;
    }
    try {
        context.input.read(static_cast<char*>(buffer), request);
        // Some CRT read failures set eofbit as well as failbit. A short read
        // before the pinned size is an error regardless of the stream's EOF flag.
        if (context.input.gcount() == request && !context.input.fail()) {
            context.delivered += static_cast<std::uint64_t>(request);
            return static_cast<int>(request);
        }
    } catch (...) {
        // Never unwind C++ exceptions through Lhasa callbacks.
    }
    context.failed = true;
    return -1;
}

const LHAInputStreamType kFlitzZipLhaInputStreamType{
    lha_stream_read,
    // Consume skipped bytes through the bounded read path: seekg can succeed
    // beyond EOF and does not prove that a declared compressed extent exists.
    nullptr,
    nullptr,
};

class LhaReaderSession {
  public:
    // Purpose: Open one LHA/LZH archive and bind it to a Lhasa reader.
    // Inputs: `source` pins the exact archive object and byte extent throughout this session.
    // Outputs: Owns the C stream/reader state or throws on open/allocation failure.
    explicit LhaReaderSession(const PinnedSourceFile& source) {
        input_ = std::make_unique<LhaInputContext>();
        input_->size = source.size();
        input_->input.open(source.path(), std::ios::binary);
        if (!input_->input) {
            throw ArchiveError("cannot open LHA archive: " + path_diagnostic_utf8(source.path()));
        }
        stream_ = lha_input_stream_new(&kFlitzZipLhaInputStreamType, input_.get());
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

    // Purpose: Advance only across an explicitly validated LHA member boundary.
    // Inputs: The session's current header/payload position.
    // Outputs: Returns a borrowed header, nullptr for permitted termination, or throws on any reader error.
    LHAFileHeader* next_header() {
        LHAFileHeader* header = nullptr;
        const auto result = lha_reader_next_file(reader_, &header);
        if (result == LHA_READ_END) {
            return nullptr;
        }
        if (result != LHA_READ_OK || header == nullptr) {
            throw ArchiveError("LHA header or compressed extent is truncated, malformed, or unreadable");
        }
        return header;
    }

  private:
    std::unique_ptr<LhaInputContext> input_;
    LHAInputStream* stream_ = nullptr;
    LHAReader* reader_ = nullptr;
};

// Purpose: Return whether an LHA header represents a directory.
// Inputs: `header` is the current Lhasa header pointer.
// Outputs: Returns true for directory members and false for regular payloads.
bool lha_header_is_directory(const LHAFileHeader& header) {
    return std::strcmp(header.compress_method, LHA_COMPRESS_TYPE_DIR) == 0 && header.symlink_target == nullptr;
}

// Purpose: Copy one required NUL-terminated Lhasa metadata string under an explicit byte bound.
// Inputs: `value` is null or library-owned text, `max_bytes` is the permitted length, and `label` names diagnostics.
// Outputs: Returns a bounded copy or throws without reading beyond `max_bytes + 1` bytes.
std::string bounded_lha_string(const char* value, std::size_t max_bytes, const char* label) {
    if (value == nullptr) {
        return {};
    }
    std::size_t length = 0;
    while (length <= max_bytes && value[length] != '\0') {
        ++length;
    }
    if (length > max_bytes) {
        throw ArchiveError(std::string("LHA ") + label + " exceeds FlitzZip path limits");
    }
    return std::string(value, length);
}

// Purpose: Build the full archive path from Lhasa's split path/name fields.
// Inputs: `header` is untrusted archive metadata decoded by Lhasa.
// Outputs: Returns the raw archive path string for strict FlitzZip normalization.
std::string lha_header_path(const LHAFileHeader& header) {
    if (header.symlink_target != nullptr) {
        throw SecurityError("LHA symbolic links are not supported");
    }

    std::string path = bounded_lha_string(header.path, kMaxArchivePathBytes, "directory path");
    const std::string filename = bounded_lha_string(header.filename, kMaxArchivePathComponentBytes, "filename");
    if (filename.empty()) {
        return path;
    }
    if (!path.empty() && path.back() != '/') {
        if (path.size() >= kMaxArchivePathBytes) {
            throw ArchiveError("LHA member path exceeds FlitzZip path limits");
        }
        path.push_back('/');
    }
    if (filename.size() > kMaxArchivePathBytes - path.size()) {
        throw ArchiveError("LHA member path exceeds FlitzZip path limits");
    }
    path.append(filename);
    return path;
}

// Purpose: Validate every LHA member path and payload before destination writes start.
// Inputs: `source` pins the archive object and seals its physical byte extent for the complete scan.
// Outputs: Returns safe metadata; throws on parser failure, unsafe path, unsupported entry type, CRC mismatch, or
// resource exhaustion.
LhaMetadata scan_lha_metadata(const PinnedSourceFile& source) {
    LhaReaderSession session(source);
    LhaMetadata metadata;
    std::vector<ArchivePathValidationEntry> validation_entries;
    std::uint32_t entry_count = 0;

    for (;;) {
        LHAFileHeader* header = session.next_header();
        if (header == nullptr) {
            break;
        }
        if (entry_count >= kMaxArchiveEntries) {
            throw ArchiveError("LHA entry count exceeds FlitzZip resource limit");
        }
        ++entry_count;

        const bool directory = lha_header_is_directory(*header);
        const auto normalized_path = normalize_archive_path_key(lha_header_path(*header));
        metadata.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
            metadata.path_metadata_bytes, static_cast<std::uint64_t>(normalized_path.size()) * 2U,
            "LHA retained path metadata");
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

// Purpose: Write one decoded LHA payload through FlitzZip's atomic publication path.
// Inputs: `reader` is positioned on `entry`, `destination` is the extraction root, and `overwrite` controls
// replacement. Outputs: Publishes the verified file or throws after cleanup.
void publish_lha_payload(LHAReader* reader, const LhaEntry& entry, const std::filesystem::path& destination,
                         bool overwrite) {
    const auto target = safe_join_archive_path(destination, entry.path);
    auto temporary = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary LHA extraction target: " + path_diagnostic_utf8(target));
        }

        std::array<unsigned char, kLhaReadBufferBytes> buffer{};
        std::uint64_t written = 0;
        while (written < entry.size) {
            const auto remaining = entry.size - written;
            const auto request = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            const auto decoded = lha_reader_read(reader, buffer.data(), request);
            if (decoded == 0) {
                throw ArchiveError("LHA decoder ended before expected payload size: " + entry.path);
            }
            if (decoded > request || decoded > std::numeric_limits<std::uint64_t>::max() - written) {
                throw ArchiveError("LHA decoder returned an invalid payload window: " + entry.path);
            }
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(decoded));
            if (!output) {
                throw ArchiveError("failed to write temporary LHA extraction target: " + path_diagnostic_utf8(target));
            }
            written += static_cast<std::uint64_t>(decoded);
        }
        if (lha_reader_check(reader, nullptr, nullptr) == 0) {
            throw ArchiveError("LHA payload CRC or size changed during publication: " + entry.path);
        }

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary LHA extraction target: " + path_diagnostic_utf8(target));
        }
        commit_verified_file(temporary, target, overwrite);
        cleanup_file_publish_target(temporary);
    } catch (...) {
        cleanup_file_publish_target(temporary);
        throw;
    }
}

// Purpose: Extract validated LHA entries to disk.
// Inputs: `source` pins the archive and its byte extent, `metadata` contains safe entries, `destination` is the
// extraction root, and `overwrite`/progress fields control publication behavior. Outputs: Writes directories and files
// below `destination`, or throws before leaving untracked temporary files.
void extract_lha_payloads(const PinnedSourceFile& source, const LhaMetadata& metadata,
                          const std::filesystem::path& destination, bool overwrite,
                          const ProgressCallback& progress_callback) {
    LhaReaderSession session(source);
    ProgressState progress;
    progress.start(OperationKind::Extract, metadata.total_file_bytes, metadata.entries.size());
    std::size_t metadata_index = 0;

    for (;;) {
        LHAFileHeader* header = session.next_header();
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
            create_verified_directories(safe_join_archive_path(destination, entry.path));
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

// Purpose: Extract an LHA/LZH archive through two identity-bound passes with repeated payload checks.
// Inputs: `archive_path`, `destination`, overwrite policy, and optional progress callback describe the operation.
// Outputs: Returns extraction telemetry or throws before retaining any unverified file.
OperationStats extract_lha(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    const auto metadata = scan_lha_metadata(archive_source);
    create_verified_directories(destination);
    extract_lha_payloads(archive_source, metadata, destination, overwrite, progress_callback);

    OperationStats stats;
    stats.input_bytes = archive_source.size();
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace flitzzip
