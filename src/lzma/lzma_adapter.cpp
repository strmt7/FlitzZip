#include "lzma/lzma_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

extern "C" {
#include "7zTypes.h"
#include "LzmaDec.h"
}

namespace superzip {
namespace {

constexpr std::size_t kLzmaHeaderBytes = LZMA_PROPS_SIZE + 8U;
constexpr std::size_t kLzmaInputBufferBytes = 64U * 1024U;
constexpr std::size_t kLzmaOutputBufferBytes = 64U * 1024U;
constexpr std::uint64_t kUnknownLzmaUncompressedSize = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxLzmaDictionaryBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxLzmaDecoderAllocationBytes = kMaxLzmaDictionaryBytes + 16ULL * 1024ULL * 1024ULL;

struct LzmaAloneHeader {
    std::array<Byte, LZMA_PROPS_SIZE> properties{};
    std::uint64_t declared_size = kUnknownLzmaUncompressedSize;
    std::uint32_t dictionary_size = 0;
};

struct LzmaAllocationBudget {
    std::uint64_t current_bytes = 0;
    std::unordered_map<void*, std::size_t> allocations;
};

thread_local std::shared_ptr<LzmaAllocationBudget> g_lzma_allocation_budget;

// Purpose: Read a filesystem file size into the archive telemetry type.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
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

// Purpose: Add byte counts while detecting telemetry and policy overflow.
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before unsigned wraparound/resource exhaustion.
void checked_add_lzma_output_bytes(std::uint64_t& total, std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError("LZMA output byte count overflows");
    }
    total += bytes;
    if (total > kMaxPipelineMemoryBytes) {
        throw ArchiveError("LZMA decoded output exceeds SuperZip resource limit");
    }
}

// Purpose: Convert an LZMA SDK result code to a stable SuperZip diagnostic.
// Inputs: `result` is returned by the LZMA SDK C API.
// Outputs: Returns human-readable text without throwing.
std::string lzma_result_message(SRes result) {
    switch (result) {
    case SZ_OK: return "LZMA operation completed";
    case SZ_ERROR_DATA: return "LZMA stream data is malformed";
    case SZ_ERROR_MEM: return "LZMA decoder exceeded memory limits";
    case SZ_ERROR_CRC: return "LZMA integrity check failed";
    case SZ_ERROR_UNSUPPORTED: return "LZMA stream uses unsupported properties";
    case SZ_ERROR_PARAM: return "LZMA decoder received invalid parameters";
    case SZ_ERROR_INPUT_EOF: return "LZMA stream is truncated";
    case SZ_ERROR_OUTPUT_EOF: return "LZMA decoder output ended unexpectedly";
    case SZ_ERROR_READ: return "LZMA archive read failed";
    case SZ_ERROR_WRITE: return "LZMA output write failed";
    case SZ_ERROR_PROGRESS: return "LZMA operation was cancelled";
    case SZ_ERROR_FAIL: return "LZMA decoder failed";
    case SZ_ERROR_THREAD: return "LZMA decoder thread failure";
    case SZ_ERROR_ARCHIVE: return "LZMA archive structure is malformed";
    case SZ_ERROR_NO_ARCHIVE: return "file is not an LZMA stream";
    default: return "LZMA decoder failed with SDK result " + std::to_string(result);
    }
}

// Purpose: Throw the SuperZip error that corresponds to an SDK result.
// Inputs: `result` is the SDK status and `context` names the operation.
// Outputs: Throws `ArchiveError` unless `result` is `SZ_OK`.
void throw_on_lzma_error(SRes result, const char* context) {
    if (result != SZ_OK) {
        throw ArchiveError(std::string(context) + ": " + lzma_result_message(result));
    }
}

// Purpose: Allocate bounded memory for the LZMA SDK decoder.
// Inputs: `size` is the SDK allocation request.
// Outputs: Returns a zero-initialized C-heap allocation or null when the request exceeds policy.
void* lzma_alloc(ISzAllocPtr, std::size_t size) {
    const auto bytes = size == 0U ? 1U : size;
    const auto budget = g_lzma_allocation_budget;
    if (budget && (bytes > kMaxLzmaDecoderAllocationBytes ||
        budget->current_bytes > kMaxLzmaDecoderAllocationBytes - bytes)) {
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

// Purpose: Free memory allocated by `lzma_alloc`.
// Inputs: `address` is null or a pointer returned by the SDK allocator.
// Outputs: Releases memory and updates the active bounded allocation budget.
void lzma_free(ISzAllocPtr, void* address) {
    if (address == nullptr) {
        return;
    }
    const auto budget = g_lzma_allocation_budget;
    if (budget) {
        const auto it = budget->allocations.find(address);
        if (it != budget->allocations.end()) {
            budget->current_bytes -= static_cast<std::uint64_t>(it->second);
            budget->allocations.erase(it);
        }
    }
    std::free(address);
}

class ScopedLzmaAllocationBudget {
public:
    // Purpose: Install a bounded allocation budget for SDK callbacks on the current thread.
    // Inputs: None.
    // Outputs: Restores any previous allocator budget when destroyed.
    ScopedLzmaAllocationBudget()
        : previous_(std::move(g_lzma_allocation_budget)) {
        g_lzma_allocation_budget = std::make_shared<LzmaAllocationBudget>();
    }

    ScopedLzmaAllocationBudget(const ScopedLzmaAllocationBudget&) = delete;
    ScopedLzmaAllocationBudget& operator=(const ScopedLzmaAllocationBudget&) = delete;

    // Purpose: Restore the prior thread-local allocator budget.
    // Inputs: None.
    // Outputs: Leaves the current thread in its previous allocator state.
    ~ScopedLzmaAllocationBudget() {
        g_lzma_allocation_budget = std::move(previous_);
    }

private:
    std::shared_ptr<LzmaAllocationBudget> previous_;
};

class ScopedLzmaDecoder {
public:
    // Purpose: Allocate an LZMA decoder for one parsed LZMA-Alone header.
    // Inputs: `properties` are the five LZMA property bytes from the stream header.
    // Outputs: Owns initialized SDK decoder state or throws on unsupported properties/allocation limits.
    explicit ScopedLzmaDecoder(const std::array<Byte, LZMA_PROPS_SIZE>& properties) {
        LzmaDec_Construct(&decoder_);
        throw_on_lzma_error(LzmaDec_Allocate(&decoder_, properties.data(), LZMA_PROPS_SIZE, &allocator_), "LZMA decoder allocation failed");
        allocated_ = true;
        LzmaDec_Init(&decoder_);
    }

    ScopedLzmaDecoder(const ScopedLzmaDecoder&) = delete;
    ScopedLzmaDecoder& operator=(const ScopedLzmaDecoder&) = delete;

    // Purpose: Release SDK decoder allocations.
    // Inputs: None.
    // Outputs: Frees the LZMA probability and dictionary buffers.
    ~ScopedLzmaDecoder() {
        if (allocated_) {
            LzmaDec_Free(&decoder_, &allocator_);
        }
    }

    // Purpose: Return the mutable SDK decoder state.
    // Inputs: None.
    // Outputs: Returns a reference suitable for LzmaDec_DecodeToBuf.
    CLzmaDec& get() {
        return decoder_;
    }

private:
    ISzAlloc allocator_{lzma_alloc, lzma_free};
    CLzmaDec decoder_{};
    bool allocated_ = false;
};

// Purpose: Derive a safe single output entry name from the archive filename.
// Inputs: `archive_path` is the host path to the `.lzma` stream.
// Outputs: Returns a relative archive entry name that can pass path-safety checks.
std::string lzma_output_entry_name(const std::filesystem::path& archive_path) {
    auto filename = archive_path.filename().string();
    auto lower = filename;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.size() > 5U && lower.ends_with(".lzma")) {
        filename.resize(filename.size() - 5U);
    } else {
        filename = archive_path.stem().string();
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

// Purpose: Parse and validate the fixed LZMA-Alone stream header.
// Inputs: `input` is positioned at the start of the archive payload.
// Outputs: Returns decoded properties, dictionary size, and declared uncompressed size.
LzmaAloneHeader read_lzma_alone_header(std::ifstream& input) {
    std::array<Byte, kLzmaHeaderBytes> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        throw ArchiveError("LZMA stream is too small to contain a complete header");
    }
    if (!input && !input.eof()) {
        throw ArchiveError("failed to read LZMA stream header");
    }

    LzmaAloneHeader parsed;
    std::copy_n(header.begin(), LZMA_PROPS_SIZE, parsed.properties.begin());
    CLzmaProps properties{};
    throw_on_lzma_error(LzmaProps_Decode(&properties, parsed.properties.data(), LZMA_PROPS_SIZE), "LZMA properties are invalid");
    parsed.dictionary_size = properties.dicSize;
    if (parsed.dictionary_size > kMaxLzmaDictionaryBytes) {
        throw ArchiveError("LZMA dictionary exceeds SuperZip resource limit");
    }

    std::uint64_t declared = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        declared |= static_cast<std::uint64_t>(header[LZMA_PROPS_SIZE + i]) << (8U * i);
    }
    parsed.declared_size = declared;
    if (declared != kUnknownLzmaUncompressedSize && declared > kMaxPipelineMemoryBytes) {
        throw ArchiveError("LZMA declared output size exceeds SuperZip resource limit");
    }
    return parsed;
}

// Purpose: Refill the compressed input buffer when all currently buffered bytes were consumed.
// Inputs: `input` is the archive stream and buffer state is updated in place.
// Outputs: Returns true when bytes are available after refill; false only at clean input EOF.
bool refill_lzma_input(
    std::ifstream& input,
    std::array<Byte, kLzmaInputBufferBytes>& buffer,
    std::size_t& position,
    std::size_t& size,
    bool& eof) {
    if (position < size) {
        return true;
    }
    if (eof) {
        return false;
    }
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count < 0) {
        throw ArchiveError("failed to read LZMA stream payload");
    }
    position = 0;
    size = static_cast<std::size_t>(count);
    if (size < buffer.size()) {
        if (input.bad()) {
            throw ArchiveError("failed to read LZMA stream payload");
        }
        eof = true;
    }
    return size > 0U;
}

// Purpose: Return whether unread compressed bytes remain after an end marker.
// Inputs: `input` is the archive stream and buffer state describes already-read data.
// Outputs: Returns true when the archive has trailing bytes after decoder completion.
bool has_lzma_trailing_bytes(
    std::ifstream& input,
    std::size_t position,
    std::size_t size,
    bool eof) {
    if (position < size) {
        return true;
    }
    if (eof) {
        return false;
    }
    return input.peek() != std::char_traits<char>::eof();
}

// Purpose: Decode the LZMA-Alone payload into one private temporary output file.
// Inputs: `input` is positioned immediately after the 13-byte header, `header` contains parsed policy-relevant metadata, `temporary_file` receives decoded bytes, and `target` is used only for diagnostics.
// Outputs: Returns decoded byte count; throws before returning on malformed data, policy violations, or failed writes.
std::uint64_t decode_lzma_stream_to_file(
    std::ifstream& input,
    const LzmaAloneHeader& header,
    const std::filesystem::path& temporary_file,
    const std::filesystem::path& target) {
    std::uint64_t output_size = 0;
    ScopedLzmaAllocationBudget allocation_budget;
    ScopedLzmaDecoder decoder(header.properties);
    std::ofstream output(temporary_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ArchiveError("cannot create LZMA extraction target: " + target.string());
    }

    std::array<Byte, kLzmaInputBufferBytes> input_buffer{};
    std::array<Byte, kLzmaOutputBufferBytes> output_buffer{};
    std::size_t input_position = 0;
    std::size_t input_size = 0;
    bool input_eof = false;
    bool finished = false;

    while (!finished) {
        const bool has_input = refill_lzma_input(input, input_buffer, input_position, input_size, input_eof);
        if (!has_input) {
            if (header.declared_size != kUnknownLzmaUncompressedSize && output_size == header.declared_size) {
                break;
            }
            throw ArchiveError("LZMA stream ended before decoder completion");
        }

        std::uint64_t remaining_declared = static_cast<std::uint64_t>(output_buffer.size());
        if (header.declared_size != kUnknownLzmaUncompressedSize) {
            if (output_size > header.declared_size) {
                throw ArchiveError("LZMA stream produced more bytes than declared");
            }
            remaining_declared = header.declared_size - output_size;
            if (remaining_declared == 0U) {
                break;
            }
        }

        SizeT destination_length = static_cast<SizeT>(std::min<std::uint64_t>(output_buffer.size(), remaining_declared));
        SizeT source_length = static_cast<SizeT>(input_size - input_position);
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        const SRes result = LzmaDec_DecodeToBuf(
            &decoder.get(),
            output_buffer.data(),
            &destination_length,
            input_buffer.data() + input_position,
            &source_length,
            LZMA_FINISH_ANY,
            &status);
        throw_on_lzma_error(result, "LZMA decode failed");
        input_position += static_cast<std::size_t>(source_length);

        if (destination_length > 0U) {
            checked_add_lzma_output_bytes(output_size, static_cast<std::uint64_t>(destination_length));
            if (header.declared_size != kUnknownLzmaUncompressedSize && output_size > header.declared_size) {
                throw ArchiveError("LZMA stream produced more bytes than declared");
            }
            output.write(reinterpret_cast<const char*>(output_buffer.data()), static_cast<std::streamsize>(destination_length));
            if (!output) {
                throw ArchiveError("failed to write LZMA extraction target: " + target.string());
            }
        }

        if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
            if (header.declared_size != kUnknownLzmaUncompressedSize && output_size != header.declared_size) {
                throw ArchiveError("LZMA end marker appeared before declared output size");
            }
            if (has_lzma_trailing_bytes(input, input_position, input_size, input_eof)) {
                throw ArchiveError("LZMA stream contains trailing data after end marker");
            }
            finished = true;
        } else if (destination_length == 0U && source_length == 0U) {
            throw ArchiveError("LZMA decoder made no forward progress");
        } else if (header.declared_size != kUnknownLzmaUncompressedSize && output_size == header.declared_size) {
            finished = true;
        }
    }

    if (header.declared_size != kUnknownLzmaUncompressedSize && output_size != header.declared_size) {
        throw ArchiveError("LZMA decoded output size does not match header declaration");
    }
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize LZMA extraction target: " + target.string());
    }
    return output_size;
}

}  // namespace

// Purpose: Extract one legacy LZMA-Alone stream into a single verified output file.
// Inputs: `archive_path` is the source `.lzma` stream, `destination` is the extraction root, `overwrite` controls existing targets, and `progress_callback` receives progress snapshots.
// Outputs: Returns extraction telemetry; throws on malformed streams, unsafe output names, refused overwrite, resource limits, or publish failures.
OperationStats extract_lzma_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open LZMA archive: " + archive_path.string());
    }
    const auto header = read_lzma_alone_header(input);
    const auto entry_name = lzma_output_entry_name(archive_path);

    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing LZMA extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        output_size = decode_lzma_stream_to_file(input, header, temporary.file, target);
        progress.add_bytes(archive_size);
        publish_progress(progress, progress_callback);
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
