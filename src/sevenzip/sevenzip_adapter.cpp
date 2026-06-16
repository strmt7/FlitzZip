#include "sevenzip/sevenzip_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "7z.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zTypes.h"
}

namespace superzip {
namespace {

constexpr std::size_t kSevenZipInputBufferBytes = 256U * 1024U;
constexpr std::size_t kMaxSevenZipNameUtf16Units = 64U * 1024U;
constexpr std::uint32_t kMaxSevenZipEntries = 1U * 1024U * 1024U;
constexpr std::uint64_t kMaxSevenZipTotalFileBytes = kMaxPipelineMemoryBytes;
constexpr std::uint64_t kMaxSevenZipDecoderAllocationBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

struct SevenZipEntry {
    UInt32 index = 0;
    std::string path;
    bool directory = false;
    std::uint64_t size = 0;
};

struct SevenZipMetadata {
    std::vector<SevenZipEntry> entries;
    std::uint64_t total_file_bytes = 0;
};

struct SevenZipAllocationBudget {
    std::uint64_t current_bytes = 0;
    std::unordered_map<void*, std::size_t> allocations;
};

thread_local std::shared_ptr<SevenZipAllocationBudget> g_sevenzip_allocation_budget;

// Purpose: Convert an SDK result code to a stable SuperZip diagnostic.
// Inputs: `result` is returned by the LZMA SDK C API.
// Outputs: Returns human-readable text without throwing.
std::string sevenzip_result_message(SRes result) {
    switch (result) {
    case SZ_OK: return "7z operation completed";
    case SZ_ERROR_DATA: return "7z archive data is malformed";
    case SZ_ERROR_MEM: return "7z decoder exceeded memory limits";
    case SZ_ERROR_CRC: return "7z payload CRC check failed";
    case SZ_ERROR_UNSUPPORTED: return "7z archive uses an unsupported compression, encryption, or metadata feature";
    case SZ_ERROR_PARAM: return "7z decoder received invalid parameters";
    case SZ_ERROR_INPUT_EOF: return "7z archive is truncated";
    case SZ_ERROR_OUTPUT_EOF: return "7z decoder output ended unexpectedly";
    case SZ_ERROR_READ: return "7z archive read failed";
    case SZ_ERROR_WRITE: return "7z output write failed";
    case SZ_ERROR_PROGRESS: return "7z operation was cancelled";
    case SZ_ERROR_THREAD: return "7z decoder thread failure";
    case SZ_ERROR_ARCHIVE: return "7z archive structure is malformed";
    case SZ_ERROR_NO_ARCHIVE: return "file is not a 7z archive";
    default: return "7z decoder failed with SDK result " + std::to_string(result);
    }
}

// Purpose: Throw the SuperZip error that corresponds to an SDK result.
// Inputs: `result` is the SDK status and `context` names the operation.
// Outputs: Throws `ArchiveError` unless `result` is `SZ_OK`.
void throw_on_7z_error(SRes result, const char* context) {
    if (result != SZ_OK) {
        throw ArchiveError(std::string(context) + ": " + sevenzip_result_message(result));
    }
}

// Purpose: Add a 7z file size to an archive total with overflow and resource checks.
// Inputs: `total` is the running byte count and `size` is the next file size.
// Outputs: Returns the updated total or throws before overflow/resource exhaustion.
std::uint64_t checked_add_7z_bytes(std::uint64_t total, std::uint64_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError("7z uncompressed payload byte count overflows");
    }
    total += size;
    if (total > kMaxSevenZipTotalFileBytes) {
        throw ArchiveError("7z uncompressed payload exceeds SuperZip resource limit");
    }
    return total;
}

// Purpose: Allocate bounded memory for the LZMA SDK 7z decoder.
// Inputs: `size` is the SDK allocation request.
// Outputs: Returns a zero-initialized C-heap allocation or null when the request exceeds policy.
void* sevenzip_alloc(ISzAllocPtr, std::size_t size) {
    const auto bytes = size == 0 ? 1U : size;
    const auto budget = g_sevenzip_allocation_budget;
    if (budget && (bytes > kMaxSevenZipDecoderAllocationBytes ||
        budget->current_bytes > kMaxSevenZipDecoderAllocationBytes - bytes)) {
        return nullptr;
    }
    void* allocation = std::calloc(1U, bytes);
    if (allocation == nullptr) {
        return nullptr;
    }
    if (!budget) {
        return allocation;
    }
    try {
        budget->allocations.emplace(allocation, bytes);
        budget->current_bytes += static_cast<std::uint64_t>(bytes);
    } catch (...) {
        std::free(allocation);
        return nullptr;
    }
    return allocation;
}

// Purpose: Free memory allocated by `sevenzip_alloc`.
// Inputs: `address` is null or a pointer returned by the SDK allocator.
// Outputs: Releases memory and updates the active bounded allocation budget.
void sevenzip_free(ISzAllocPtr, void* address) {
    if (address == nullptr) {
        return;
    }
    const auto budget = g_sevenzip_allocation_budget;
    if (budget) {
        const auto it = budget->allocations.find(address);
        if (it != budget->allocations.end()) {
            budget->current_bytes -= static_cast<std::uint64_t>(it->second);
            budget->allocations.erase(it);
        }
    }
    std::free(address);
}

class ScopedSevenZipAllocationBudget {
public:
    // Purpose: Install a bounded allocation budget for SDK callbacks on the current thread.
    // Inputs: None.
    // Outputs: Restores any previous allocator budget when destroyed.
    ScopedSevenZipAllocationBudget()
        : previous_(std::move(g_sevenzip_allocation_budget)) {
        g_sevenzip_allocation_budget = std::make_shared<SevenZipAllocationBudget>();
    }

    ScopedSevenZipAllocationBudget(const ScopedSevenZipAllocationBudget&) = delete;
    ScopedSevenZipAllocationBudget& operator=(const ScopedSevenZipAllocationBudget&) = delete;

    // Purpose: Restore the prior thread-local allocator budget.
    // Inputs: None.
    // Outputs: Leaves the current thread in its previous allocator state.
    ~ScopedSevenZipAllocationBudget() {
        g_sevenzip_allocation_budget = std::move(previous_);
    }

private:
    std::shared_ptr<SevenZipAllocationBudget> previous_;
};

struct SevenZipFileStream {
    ISeekInStream vt{};
    std::ifstream input;
    std::uint64_t size = 0;
};

// Purpose: Recover the owning stream from an SDK stream vtable pointer.
// Inputs: `stream` points to the first member of `SevenZipFileStream`.
// Outputs: Returns the owning C++ stream object.
SevenZipFileStream& sevenzip_stream_from_vtable(ISeekInStreamPtr stream) {
    return *reinterpret_cast<SevenZipFileStream*>(const_cast<ISeekInStream*>(stream));
}

// Purpose: Read bytes for the LZMA SDK from a C++ input stream.
// Inputs: `stream` is the SDK stream pointer, `buffer` receives bytes, and `size` is in/out byte count.
// Outputs: Mutates `size` with bytes read and returns an SDK status.
SRes sevenzip_stream_read(ISeekInStreamPtr stream, void* buffer, std::size_t* size) {
    auto& self = sevenzip_stream_from_vtable(stream);
    const auto requested = *size;
    if (requested == 0) {
        return SZ_OK;
    }
    self.input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(requested));
    *size = static_cast<std::size_t>(std::max<std::streamsize>(0, self.input.gcount()));
    if (*size == requested || self.input.eof()) {
        self.input.clear();
        return SZ_OK;
    }
    return SZ_ERROR_READ;
}

// Purpose: Seek the LZMA SDK input stream.
// Inputs: `stream` is the SDK stream pointer, `position` is in/out offset, and `origin` is the seek origin.
// Outputs: Mutates `position` with the resulting absolute offset and returns an SDK status.
SRes sevenzip_stream_seek(ISeekInStreamPtr stream, Int64* position, ESzSeek origin) {
    auto& self = sevenzip_stream_from_vtable(stream);
    std::uint64_t base = 0;
    if (origin == SZ_SEEK_CUR) {
        const auto current = self.input.tellg();
        if (current < 0) {
            return SZ_ERROR_READ;
        }
        base = static_cast<std::uint64_t>(current);
    } else if (origin == SZ_SEEK_END) {
        base = self.size;
    } else if (origin != SZ_SEEK_SET) {
        return SZ_ERROR_PARAM;
    }
    const auto requested = *position;
    if (requested < 0 && static_cast<std::uint64_t>(-requested) > base) {
        return SZ_ERROR_PARAM;
    }
    const auto target = requested < 0
        ? base - static_cast<std::uint64_t>(-requested)
        : base + static_cast<std::uint64_t>(requested);
    if (target > self.size || target > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return SZ_ERROR_PARAM;
    }
    self.input.clear();
    self.input.seekg(static_cast<std::streamoff>(target), std::ios::beg);
    if (!self.input) {
        return SZ_ERROR_READ;
    }
    *position = static_cast<Int64>(target);
    return SZ_OK;
}

class SevenZipArchive {
public:
    // Purpose: Open a 7z archive and parse its database through the LZMA SDK.
    // Inputs: `archive_path` identifies the archive to read.
    // Outputs: Owns SDK stream/database state or throws on open/parse failure.
    explicit SevenZipArchive(const std::filesystem::path& archive_path) {
        try {
            file_.input.open(archive_path, std::ios::binary);
            if (!file_.input) {
                throw ArchiveError("cannot open 7z archive: " + archive_path.string());
            }
            file_.input.seekg(0, std::ios::end);
            const auto end = file_.input.tellg();
            if (end < 0) {
                throw ArchiveError("failed to determine 7z archive size: " + archive_path.string());
            }
            file_.size = static_cast<std::uint64_t>(end);
            file_.input.seekg(0, std::ios::beg);
            file_.vt.Read = sevenzip_stream_read;
            file_.vt.Seek = sevenzip_stream_seek;

            LookToRead2_CreateVTable(&look_stream_, False);
            look_stream_.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocator_, kSevenZipInputBufferBytes));
            if (look_stream_.buf == nullptr) {
                throw ArchiveError("7z decoder input buffer exceeds memory limits");
            }
            look_stream_.bufSize = kSevenZipInputBufferBytes;
            look_stream_.realStream = &file_.vt;
            LookToRead2_INIT(&look_stream_);

            std::call_once(crc_once_, []() {
                CrcGenerateTable();
            });
            SzArEx_Init(&database_);
            database_initialized_ = true;
            throw_on_7z_error(SzArEx_Open(&database_, &look_stream_.vt, &allocator_, &allocator_), "failed to open 7z archive");
        } catch (...) {
            cleanup();
            throw;
        }
    }

    SevenZipArchive(const SevenZipArchive&) = delete;
    SevenZipArchive& operator=(const SevenZipArchive&) = delete;

    // Purpose: Release SDK archive resources.
    // Inputs: None.
    // Outputs: Frees decoder buffers and closes the archive stream.
    ~SevenZipArchive() {
        cleanup();
    }

    // Purpose: Release all SDK-owned archive resources without throwing.
    // Inputs: None.
    // Outputs: Leaves the wrapper in an empty state suitable for destruction.
    void cleanup() noexcept {
        if (out_buffer_ != nullptr) {
            ISzAlloc_Free(&allocator_, out_buffer_);
            out_buffer_ = nullptr;
        }
        if (database_initialized_) {
            SzArEx_Free(&database_, &allocator_);
            database_initialized_ = false;
        }
        if (look_stream_.buf != nullptr) {
            ISzAlloc_Free(&allocator_, look_stream_.buf);
            look_stream_.buf = nullptr;
        }
    }

    // Purpose: Return the parsed SDK database.
    // Inputs: None.
    // Outputs: Returns a mutable SDK database reference for metadata and extraction.
    CSzArEx& database() {
        return database_;
    }

    // Purpose: Extract one file into the SDK-owned solid-block cache.
    // Inputs: `index` is the 7z file index.
    // Outputs: Returns the decoded byte window for that file or throws on decoder failure.
    std::span<const std::byte> extract_to_cache(UInt32 index) {
        std::size_t offset = 0;
        std::size_t size = 0;
        throw_on_7z_error(
            SzArEx_Extract(
                &database_,
                &look_stream_.vt,
                index,
                &block_index_,
                &out_buffer_,
                &out_buffer_size_,
                &offset,
                &size,
                &allocator_,
                &allocator_),
            "failed to extract 7z member");
        if (offset > out_buffer_size_ || size > out_buffer_size_ - offset) {
            throw ArchiveError("7z decoder returned an invalid output window");
        }
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(out_buffer_ + offset),
            size);
    }

    // Purpose: Reset the SDK solid-block cache between validation and extraction passes.
    // Inputs: None.
    // Outputs: Frees cached decoded blocks so a later pass starts from a clean state.
    void reset_cache() {
        if (out_buffer_ != nullptr) {
            ISzAlloc_Free(&allocator_, out_buffer_);
            out_buffer_ = nullptr;
        }
        out_buffer_size_ = 0;
        block_index_ = 0xFFFFFFFFU;
    }

private:
    static std::once_flag crc_once_;

    const ISzAlloc allocator_{sevenzip_alloc, sevenzip_free};
    SevenZipFileStream file_{};
    CLookToRead2 look_stream_{};
    CSzArEx database_{};
    bool database_initialized_ = false;
    UInt32 block_index_ = 0xFFFFFFFFU;
    Byte* out_buffer_ = nullptr;
    std::size_t out_buffer_size_ = 0;
};

std::once_flag SevenZipArchive::crc_once_;

// Purpose: Convert a UTF-16 code point to UTF-8.
// Inputs: `code_point` is a valid Unicode scalar value.
// Outputs: Appends UTF-8 bytes to `output`.
void append_utf8_code_point(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// Purpose: Convert a NUL-terminated SDK UTF-16 filename to UTF-8.
// Inputs: `name` contains UTF-16 code units from 7z metadata.
// Outputs: Returns UTF-8 text or throws on invalid surrogate pairs.
std::string utf16_name_to_utf8(const std::vector<UInt16>& name) {
    std::string output;
    std::size_t i = 0;
    while (i < name.size() && name[i] != 0) {
        std::uint32_t code_point = name[i];
        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            if ((i + 1U) >= name.size() || name[i + 1U] < 0xDC00U || name[i + 1U] > 0xDFFFU) {
                throw SecurityError("7z filename contains an invalid UTF-16 surrogate pair");
            }
            code_point = 0x10000U + (((code_point - 0xD800U) << 10U) | (name[i + 1U] - 0xDC00U));
            i += 2U;
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            throw SecurityError("7z filename contains an unpaired UTF-16 low surrogate");
        } else {
            ++i;
        }
        append_utf8_code_point(output, code_point);
    }
    return output;
}

// Purpose: Return a validated UTF-8 path for a 7z entry.
// Inputs: `database` is the parsed SDK archive and `index` identifies an entry.
// Outputs: Returns the raw archive path converted to UTF-8 before SuperZip normalization.
std::string sevenzip_entry_path(const CSzArEx& database, UInt32 index) {
    const auto units = SzArEx_GetFileNameUtf16(&database, index, nullptr);
    if (units == 0 || units > kMaxSevenZipNameUtf16Units) {
        throw ArchiveError("7z filename exceeds SuperZip resource limit");
    }
    std::vector<UInt16> name(units);
    const auto written = SzArEx_GetFileNameUtf16(&database, index, name.data());
    if (written == 0 || written > name.size() || name.back() != 0) {
        throw ArchiveError("7z filename metadata is malformed");
    }
    return utf16_name_to_utf8(name);
}

// Purpose: Detect 7z attributes that SuperZip does not safely publish.
// Inputs: `database` is the parsed SDK archive, `index` identifies the entry, and `directory` is the decoded entry kind.
// Outputs: Throws for reparse points, devices, or POSIX special-file modes.
void reject_unsupported_7z_attributes(const CSzArEx& database, UInt32 index, bool directory) {
    if (!SzBitWithVals_Check(&database.Attribs, index)) {
        return;
    }
    const auto attributes = database.Attribs.Vals[index];
    constexpr UInt32 kWindowsFileAttributeDevice = 0x00000040U;
    constexpr UInt32 kWindowsFileAttributeReparsePoint = 0x00000400U;
    constexpr UInt32 unsupported_windows = kWindowsFileAttributeDevice | kWindowsFileAttributeReparsePoint;
    if ((attributes & unsupported_windows) != 0) {
        throw SecurityError("7z entry uses unsupported Windows special-file attributes");
    }
    if ((attributes & 0xF0000000U) != 0) {
        const auto posix_type = attributes & 0xF0000000U;
        const auto expected = directory ? 0x40000000U : 0x80000000U;
        if (posix_type != expected) {
            throw SecurityError("7z entry uses unsupported POSIX special-file attributes");
        }
    }
}

// Purpose: Build and validate SuperZip metadata from a parsed 7z database.
// Inputs: `database` is the parsed SDK archive.
// Outputs: Returns all entries and total output size after archive-wide path validation.
SevenZipMetadata scan_7z_metadata(CSzArEx& database) {
    if (database.NumFiles > kMaxSevenZipEntries) {
        throw ArchiveError("7z entry count exceeds SuperZip resource limit");
    }
    SevenZipMetadata metadata;
    metadata.entries.reserve(database.NumFiles);
    std::vector<ArchivePathValidationEntry> validation_entries;
    validation_entries.reserve(database.NumFiles);
    for (UInt32 index = 0; index < database.NumFiles; ++index) {
        const bool directory = SzArEx_IsDir(&database, index) != 0;
        reject_unsupported_7z_attributes(database, index, directory);
        const auto path = sevenzip_entry_path(database, index);
        const auto normalized = normalize_archive_path_key(path);
        const auto size = directory ? 0U : SzArEx_GetFileSize(&database, index);
        if (!directory) {
            metadata.total_file_bytes = checked_add_7z_bytes(metadata.total_file_bytes, size);
        }
        metadata.entries.push_back(SevenZipEntry{
            .index = index,
            .path = normalized,
            .directory = directory,
            .size = static_cast<std::uint64_t>(size),
        });
        validation_entries.push_back(ArchivePathValidationEntry{
            .path = normalized,
            .directory = directory,
        });
    }
    validate_archive_path_set(validation_entries);
    return metadata;
}

// Purpose: Validate every 7z file payload before destination writes start.
// Inputs: `archive` owns the SDK state and `metadata` lists validated entries.
// Outputs: Throws on decode/CRC failure or size mismatch.
void validate_7z_payloads(SevenZipArchive& archive, const SevenZipMetadata& metadata) {
    for (const auto& entry : metadata.entries) {
        if (entry.directory) {
            continue;
        }
        const auto payload = archive.extract_to_cache(entry.index);
        if (payload.size() != entry.size) {
            throw ArchiveError("7z decoder output size does not match validated metadata: " + entry.path);
        }
    }
    archive.reset_cache();
}

// Purpose: Write one decoded 7z payload through SuperZip's atomic publication path.
// Inputs: `destination` is the extraction root, `entry` names the target, `payload` contains decoded bytes, and `overwrite` controls replacement.
// Outputs: Publishes the verified file or throws after cleanup.
void publish_7z_payload(
    const std::filesystem::path& destination,
    const SevenZipEntry& entry,
    std::span<const std::byte> payload,
    bool overwrite) {
    const auto target = safe_join_archive_path(destination, entry.path);
    std::filesystem::create_directories(target.parent_path());
    auto temporary = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary 7z extraction target: " + target.string());
        }
        output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!output) {
            throw ArchiveError("failed to write temporary 7z extraction target: " + target.string());
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary 7z extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
    } catch (...) {
        cleanup_file_publish_target(temporary);
        throw;
    }
}

// Purpose: Extract validated 7z entries to disk.
// Inputs: `archive` owns SDK state, `metadata` contains safe entries, `destination` is the extraction root, and `overwrite`/progress fields control publication behavior.
// Outputs: Writes directories and files below `destination`, or throws before leaving untracked temporary files.
void extract_7z_payloads(
    SevenZipArchive& archive,
    const SevenZipMetadata& metadata,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    ProgressState progress;
    progress.start(OperationKind::Extract, metadata.total_file_bytes, metadata.entries.size());
    for (const auto& entry : metadata.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
            progress.finish_entry();
            publish_progress(progress, progress_callback);
            continue;
        }
        const auto payload = archive.extract_to_cache(entry.index);
        if (payload.size() != entry.size) {
            throw ArchiveError("7z decoder output size does not match validated metadata: " + entry.path);
        }
        publish_7z_payload(destination, entry, payload, overwrite);
        progress.add_bytes(entry.size);
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
}

}  // namespace

OperationStats extract_7z(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    ScopedSevenZipAllocationBudget allocation_budget;
    SevenZipArchive archive(archive_path);
    const auto metadata = scan_7z_metadata(archive.database());
    validate_7z_payloads(archive, metadata);
    std::filesystem::create_directories(destination);
    extract_7z_payloads(archive, metadata, destination, overwrite, progress_callback);

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
