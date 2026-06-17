#include "lzip/lzip_stream.hpp"

#include "core/checksum.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <utility>

extern "C" {
#include "7zTypes.h"
#include "LzmaDec.h"
}

namespace superzip {
namespace {

constexpr std::size_t kLzipHeaderBytes = 6U;
constexpr std::size_t kLzipTrailerBytes = 20U;
constexpr std::size_t kLzipInputBufferBytes = 64U * 1024U;
constexpr std::size_t kLzipOutputBufferBytes = 64U * 1024U;
constexpr std::uint64_t kMaxLzipDictionaryBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxLzipDecoderAllocationBytes = kMaxLzipDictionaryBytes + 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxLzipMemberBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr Byte kLzipDefaultLzmaProperty = 0x5DU;

struct LzipAllocationBudget {
    std::uint64_t current_bytes = 0;
    std::unordered_map<void*, std::size_t> allocations;
};

thread_local std::shared_ptr<LzipAllocationBudget> g_lzip_allocation_budget;

// Purpose: Read a filesystem file size into a 64-bit archive counter.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
std::uint64_t lzip_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read lzip file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("lzip file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// Purpose: Convert an LZMA SDK status to an actionable lzip diagnostic.
// Inputs: `result` is returned by the LZMA SDK C API.
// Outputs: Returns human-readable text without throwing.
std::string lzip_lzma_result_message(SRes result) {
    switch (result) {
    case SZ_OK: return "LZMA operation completed";
    case SZ_ERROR_DATA: return "lzip LZMA payload is malformed";
    case SZ_ERROR_MEM: return "lzip LZMA decoder exceeded memory limits";
    case SZ_ERROR_CRC: return "lzip LZMA integrity check failed";
    case SZ_ERROR_UNSUPPORTED: return "lzip LZMA stream uses unsupported properties";
    case SZ_ERROR_PARAM: return "lzip LZMA decoder received invalid parameters";
    case SZ_ERROR_INPUT_EOF: return "lzip LZMA stream is truncated";
    case SZ_ERROR_OUTPUT_EOF: return "lzip LZMA decoder output ended unexpectedly";
    case SZ_ERROR_READ: return "lzip archive read failed";
    case SZ_ERROR_WRITE: return "lzip output write failed";
    case SZ_ERROR_PROGRESS: return "lzip operation was cancelled";
    case SZ_ERROR_FAIL: return "lzip LZMA decoder failed";
    case SZ_ERROR_THREAD: return "lzip LZMA decoder thread failure";
    case SZ_ERROR_ARCHIVE: return "lzip archive structure is malformed";
    case SZ_ERROR_NO_ARCHIVE: return "file is not a lzip stream";
    default: return "lzip LZMA decoder failed with SDK result " + std::to_string(result);
    }
}

// Purpose: Throw the SuperZip error that corresponds to an SDK result.
// Inputs: `result` is the SDK status and `context` names the operation.
// Outputs: Throws `ArchiveError` unless `result` is `SZ_OK`.
void throw_on_lzip_lzma_error(SRes result, const char* context) {
    if (result != SZ_OK) {
        throw ArchiveError(std::string(context) + ": " + lzip_lzma_result_message(result));
    }
}

// Purpose: Decode an unsigned little-endian integer from a fixed byte span.
// Inputs: `bytes` contains the field bytes in least-significant-byte first order.
// Outputs: Returns the decoded unsigned value.
std::uint64_t read_lzip_le(std::span<const unsigned char> bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
    }
    return value;
}

// Purpose: Add byte counts while enforcing stream telemetry and resource limits.
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before unsigned wraparound/resource exhaustion.
void checked_add_lzip_stream_bytes(std::uint64_t& total, std::uint64_t bytes, const char* context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    total += bytes;
    if (total > kMaxPipelineMemoryBytes) {
        throw ArchiveError(std::string(context) + " exceeds SuperZip resource limit");
    }
}

// Purpose: Decode the lzip dictionary-size byte into a raw LZMA dictionary size.
// Inputs: `coded_size` is the DS byte from the lzip member header.
// Outputs: Returns the dictionary size in bytes, or throws when the code is outside the lzip specification.
std::uint32_t decode_lzip_dictionary_size(unsigned char coded_size) {
    const auto base_log = static_cast<unsigned int>(coded_size & 0x1FU);
    const auto numerator = static_cast<unsigned int>(coded_size >> 5U);
    if (base_log < 12U || base_log > 29U) {
        throw ArchiveError("lzip dictionary size code is invalid");
    }
    const auto base = 1ULL << base_log;
    const auto dictionary = base - ((base / 16U) * numerator);
    if (dictionary < 4096ULL || dictionary > kMaxLzipDictionaryBytes) {
        throw ArchiveError("lzip dictionary size exceeds SuperZip policy");
    }
    return static_cast<std::uint32_t>(dictionary);
}

// Purpose: Build LZMA SDK properties for lzip's fixed LZMA-302eos stream profile.
// Inputs: `dictionary_size` is decoded from the lzip DS byte.
// Outputs: Returns five LZMA SDK property bytes.
std::array<Byte, LZMA_PROPS_SIZE> lzip_lzma_properties(std::uint32_t dictionary_size) {
    return {
        kLzipDefaultLzmaProperty,
        static_cast<Byte>(dictionary_size & 0xFFU),
        static_cast<Byte>((dictionary_size >> 8U) & 0xFFU),
        static_cast<Byte>((dictionary_size >> 16U) & 0xFFU),
        static_cast<Byte>((dictionary_size >> 24U) & 0xFFU),
    };
}

// Purpose: Allocate bounded memory for the LZMA SDK decoder.
// Inputs: `size` is the SDK allocation request.
// Outputs: Returns a zero-initialized C-heap allocation or null when the request exceeds policy.
void* lzip_alloc(ISzAllocPtr, std::size_t size) {
    const auto bytes = size == 0U ? 1U : size;
    const auto budget = g_lzip_allocation_budget;
    if (budget && (bytes > kMaxLzipDecoderAllocationBytes ||
        budget->current_bytes > kMaxLzipDecoderAllocationBytes - bytes)) {
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

// Purpose: Free memory allocated by `lzip_alloc`.
// Inputs: `address` is null or a pointer returned by the SDK allocator.
// Outputs: Releases memory and updates the active bounded allocation budget.
void lzip_free(ISzAllocPtr, void* address) {
    if (address == nullptr) {
        return;
    }
    const auto budget = g_lzip_allocation_budget;
    if (budget) {
        const auto it = budget->allocations.find(address);
        if (it != budget->allocations.end()) {
            budget->current_bytes -= static_cast<std::uint64_t>(it->second);
            budget->allocations.erase(it);
        }
    }
    std::free(address);
}

class ScopedLzipAllocationBudget {
public:
    // Purpose: Install a bounded allocation budget for SDK callbacks on the current thread.
    // Inputs: None.
    // Outputs: Restores any previous allocator budget when destroyed.
    ScopedLzipAllocationBudget()
        : previous_(std::move(g_lzip_allocation_budget)) {
        g_lzip_allocation_budget = std::make_shared<LzipAllocationBudget>();
    }

    ScopedLzipAllocationBudget(const ScopedLzipAllocationBudget&) = delete;
    ScopedLzipAllocationBudget& operator=(const ScopedLzipAllocationBudget&) = delete;

    // Purpose: Restore the prior thread-local allocator budget.
    // Inputs: None.
    // Outputs: Leaves the current thread in its previous allocator state.
    ~ScopedLzipAllocationBudget() {
        g_lzip_allocation_budget = std::move(previous_);
    }

private:
    std::shared_ptr<LzipAllocationBudget> previous_;
};

class ScopedLzipDecoder {
public:
    // Purpose: Allocate an LZMA decoder for one lzip member.
    // Inputs: `properties` are the synthesized LZMA property bytes from the lzip header.
    // Outputs: Owns initialized SDK decoder state or throws on unsupported properties/allocation limits.
    explicit ScopedLzipDecoder(const std::array<Byte, LZMA_PROPS_SIZE>& properties) {
        LzmaDec_Construct(&decoder_);
        throw_on_lzip_lzma_error(
            LzmaDec_Allocate(&decoder_, properties.data(), LZMA_PROPS_SIZE, &allocator_),
            "lzip LZMA decoder allocation failed");
        allocated_ = true;
        LzmaDec_Init(&decoder_);
    }

    ScopedLzipDecoder(const ScopedLzipDecoder&) = delete;
    ScopedLzipDecoder& operator=(const ScopedLzipDecoder&) = delete;

    // Purpose: Release SDK decoder allocations.
    // Inputs: None.
    // Outputs: Frees the LZMA probability and dictionary buffers.
    ~ScopedLzipDecoder() {
        if (allocated_) {
            LzmaDec_Free(&decoder_, &allocator_);
        }
    }

    // Purpose: Return the mutable SDK decoder state.
    // Inputs: None.
    // Outputs: Returns a reference suitable for `LzmaDec_DecodeToBuf`.
    CLzmaDec& get() {
        return decoder_;
    }

private:
    ISzAlloc allocator_{lzip_alloc, lzip_free};
    CLzmaDec decoder_{};
    bool allocated_ = false;
};

}  // namespace

class LzipInputStream::Buffer final : public std::streambuf {
public:
    explicit Buffer(const std::filesystem::path& archive_path)
        : input_(archive_path, std::ios::binary), archive_size_(lzip_file_size(archive_path)) {
        if (!input_) {
            throw ArchiveError("cannot open lzip stream: " + archive_path.string());
        }
        setg(
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()));
    }

    ~Buffer() override = default;

    // Purpose: Drain all remaining uncompressed bytes and force lzip trailer validation.
    // Inputs: None.
    // Outputs: Throws when any lzip member is incomplete or invalid.
    void finish() {
        while (!finished_) {
            setg(
                reinterpret_cast<char*>(output_buffer_.data()),
                reinterpret_cast<char*>(output_buffer_.data()),
                reinterpret_cast<char*>(output_buffer_.data()));
            fill_output();
        }
    }

    // Purpose: Report compressed source byte size.
    // Inputs: None.
    // Outputs: Returns the `.lz` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const {
        return archive_size_;
    }

    // Purpose: Report uncompressed byte count emitted by the decoder.
    // Inputs: None.
    // Outputs: Returns the uncompressed byte count produced so far.
    [[nodiscard]] std::uint64_t output_bytes() const {
        return output_bytes_;
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        setg(
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()));
        fill_output();
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }

private:
    // Purpose: Refill the compressed source buffer after all buffered bytes have been consumed.
    // Inputs: None.
    // Outputs: Returns true when bytes are available; false only at clean source EOF.
    bool refill_input_if_needed() {
        if (input_position_ < input_size_) {
            return true;
        }
        if (source_finished_) {
            return false;
        }
        input_.read(reinterpret_cast<char*>(input_buffer_.data()), static_cast<std::streamsize>(input_buffer_.size()));
        const auto bytes_read = static_cast<std::size_t>(input_.gcount());
        if (input_.bad()) {
            throw ArchiveError("failed to read lzip stream");
        }
        input_position_ = 0;
        input_size_ = bytes_read;
        if (bytes_read < input_buffer_.size() && input_.eof()) {
            source_finished_ = true;
        }
        return input_size_ > 0U;
    }

    // Purpose: Read exact member-scoped bytes from buffered input.
    // Inputs: `destination` receives bytes and `label` names the member region.
    // Outputs: Copies exactly `destination.size()` bytes and advances member byte counters, or throws on truncation.
    void read_member_bytes(std::span<unsigned char> destination, const char* label) {
        std::size_t written = 0;
        while (written < destination.size()) {
            if (!refill_input_if_needed()) {
                throw ArchiveError(std::string("lzip ") + label + " is truncated");
            }
            const auto available = input_size_ - input_position_;
            const auto chunk = std::min<std::size_t>(available, destination.size() - written);
            std::copy_n(input_buffer_.data() + input_position_, chunk, destination.data() + written);
            input_position_ += chunk;
            written += chunk;
            checked_add_lzip_stream_bytes(current_member_consumed_, static_cast<std::uint64_t>(chunk), "lzip member");
        }
    }

    // Purpose: Start and initialize the next lzip member when source bytes remain.
    // Inputs: None.
    // Outputs: Activates a member decoder, marks stream EOF, or throws for invalid magic/header metadata.
    void start_next_member() {
        if (!refill_input_if_needed()) {
            if (member_index_ == 0U) {
                throw ArchiveError("lzip stream contains no members");
            }
            finished_ = true;
            return;
        }
        if (last_member_was_empty_) {
            throw ArchiveError("lzip empty member is not allowed before another member");
        }

        current_member_consumed_ = 0;
        std::array<unsigned char, kLzipHeaderBytes> header{};
        read_member_bytes(header, "header");
        if (!std::equal(header.begin(), header.begin() + 4, "LZIP")) {
            throw ArchiveError("invalid lzip member magic or trailing data");
        }
        if (header[4] != 1U) {
            throw ArchiveError("unsupported lzip member version");
        }

        const auto dictionary_size = decode_lzip_dictionary_size(header[5]);
        decoder_ = std::make_unique<ScopedLzipDecoder>(lzip_lzma_properties(dictionary_size));
        current_crc32_ = 0;
        current_output_size_ = 0;
        member_active_ = true;
    }

    // Purpose: Validate the current lzip trailer against decoded member state.
    // Inputs: None; reads the 20-byte trailer from buffered source bytes.
    // Outputs: Ends the active member or throws on CRC, data-size, or member-size mismatch.
    void validate_current_member_trailer() {
        std::array<unsigned char, kLzipTrailerBytes> trailer{};
        read_member_bytes(trailer, "trailer");
        const auto expected_crc32 = static_cast<std::uint32_t>(read_lzip_le(std::span<const unsigned char>(trailer.data(), 4U)));
        const auto expected_data_size = read_lzip_le(std::span<const unsigned char>(trailer.data() + 4U, 8U));
        const auto expected_member_size = read_lzip_le(std::span<const unsigned char>(trailer.data() + 12U, 8U));
        if (expected_member_size > kMaxLzipMemberBytes) {
            throw ArchiveError("lzip member size exceeds format limit");
        }
        if (expected_member_size != current_member_consumed_) {
            throw ArchiveError("lzip member-size verification failed");
        }
        if (expected_data_size != current_output_size_) {
            throw ArchiveError("lzip data-size verification failed");
        }
        if (expected_crc32 != current_crc32_) {
            throw ArchiveError("lzip CRC32 verification failed");
        }
        last_member_was_empty_ = current_output_size_ == 0U;
        decoder_.reset();
        member_active_ = false;
        ++member_index_;
    }

    // Purpose: Fill the get area with decompressed member bytes or validate an exhausted member.
    // Inputs: None.
    // Outputs: Updates `setg` when bytes are produced; throws on malformed lzip payloads.
    void fill_output() {
        if (finished_) {
            return;
        }
        for (;;) {
            if (!member_active_) {
                start_next_member();
                if (finished_) {
                    return;
                }
            }
            if (!refill_input_if_needed()) {
                throw ArchiveError("lzip LZMA payload ended before EOS marker");
            }

            SizeT destination_length = static_cast<SizeT>(output_buffer_.size());
            SizeT source_length = static_cast<SizeT>(input_size_ - input_position_);
            ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            const SRes result = LzmaDec_DecodeToBuf(
                &decoder_->get(),
                output_buffer_.data(),
                &destination_length,
                input_buffer_.data() + input_position_,
                &source_length,
                LZMA_FINISH_ANY,
                &status);
            throw_on_lzip_lzma_error(result, "lzip LZMA decode failed");
            input_position_ += static_cast<std::size_t>(source_length);
            checked_add_lzip_stream_bytes(current_member_consumed_, static_cast<std::uint64_t>(source_length), "lzip member");

            if (destination_length > 0U) {
                current_crc32_ = crc32(
                    std::as_bytes(std::span<const Byte>(output_buffer_.data(), static_cast<std::size_t>(destination_length))),
                    current_crc32_);
                checked_add_lzip_stream_bytes(current_output_size_, static_cast<std::uint64_t>(destination_length), "lzip member output");
                checked_add_lzip_stream_bytes(output_bytes_, static_cast<std::uint64_t>(destination_length), "lzip output");
            }
            if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
                validate_current_member_trailer();
            } else if (destination_length == 0U && source_length == 0U) {
                throw ArchiveError("lzip LZMA decoder made no forward progress");
            }
            if (destination_length > 0U) {
                setg(
                    reinterpret_cast<char*>(output_buffer_.data()),
                    reinterpret_cast<char*>(output_buffer_.data()),
                    reinterpret_cast<char*>(output_buffer_.data() + destination_length));
                return;
            }
        }
    }

    std::ifstream input_;
    std::uint64_t archive_size_ = 0;
    std::uint64_t output_bytes_ = 0;
    ScopedLzipAllocationBudget allocation_budget_;
    std::unique_ptr<ScopedLzipDecoder> decoder_;
    bool source_finished_ = false;
    bool finished_ = false;
    bool member_active_ = false;
    bool last_member_was_empty_ = false;
    std::uint64_t member_index_ = 0;
    std::uint64_t current_member_consumed_ = 0;
    std::uint64_t current_output_size_ = 0;
    std::uint32_t current_crc32_ = 0;
    std::size_t input_position_ = 0;
    std::size_t input_size_ = 0;
    std::array<Byte, kLzipInputBufferBytes> input_buffer_{};
    std::array<Byte, kLzipOutputBufferBytes> output_buffer_{};
};

LzipInputStream::LzipInputStream(const std::filesystem::path& archive_path)
    : std::istream(nullptr), buffer_(std::make_unique<Buffer>(archive_path)) {
    rdbuf(buffer_.get());
}

LzipInputStream::~LzipInputStream() = default;

void LzipInputStream::finish() {
    buffer_->finish();
}

std::uint64_t LzipInputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t LzipInputStream::output_bytes() const {
    return buffer_->output_bytes();
}

}  // namespace superzip
