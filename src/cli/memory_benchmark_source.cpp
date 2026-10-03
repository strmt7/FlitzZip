#include "cli/memory_benchmark_source.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <string>

namespace superzip::cli {
namespace {
// Purpose: Generate a deterministic random-looking byte from a virtual workload offset.
// Inputs: `index` is the zero-based virtual byte offset.
// Outputs: Returns one reproducible byte without maintaining RNG state.
std::uint8_t randomish_benchmark_byte(std::uint64_t index) {
    std::uint64_t value = index + 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return static_cast<std::uint8_t>(value >> 56U);
}

// Purpose: Generate a deterministic low-entropy byte that is not a fill or periodic pattern.
// Inputs: `index` is the zero-based virtual byte offset inside the low-entropy region.
// Outputs: Returns one reproducible byte from a biased distribution similar to already-compressed scientific chunks.
std::uint8_t low_entropy_benchmark_byte(std::uint64_t index) {
    const auto bucket = static_cast<std::uint32_t>((static_cast<std::uint64_t>(randomish_benchmark_byte(index)) << 2U) |
                                                   (randomish_benchmark_byte(index + 0xA5A5A5A5ULL) & 0x03U));
    if (bucket < 180U) {
        return 1U;
    }
    if (bucket < 330U) {
        return 0U;
    }
    if (bucket < 450U) {
        return 2U;
    }
    if (bucket < 520U) {
        return 3U;
    }
    if (bucket < 720U) {
        return static_cast<std::uint8_t>(4U + (bucket % 16U));
    }
    if (bucket < 900U) {
        return static_cast<std::uint8_t>(20U + (bucket % 64U));
    }
    return static_cast<std::uint8_t>(84U + (bucket % 172U));
}

// Purpose: Fill independent 64 KiB groups whose four near-identical records require local dictionary matches.
// Inputs: `buffer` is the output and `global_offset` is its virtual file offset; neither needs group alignment.
// Outputs: Writes deterministic bytes without filesystem access or cross-group repetition.
void fill_segmented_record_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset) {
    constexpr std::size_t record_bytes = 16U * 1024U;
    constexpr std::size_t segment_bytes = 4U * record_bytes;
    constexpr std::size_t patch_offset = 1024U;
    std::array<std::byte, record_bytes> record{};
    for (std::size_t offset = 0; offset < buffer.size();) {
        const auto absolute_offset = global_offset + offset;
        const auto segment_index = absolute_offset / segment_bytes;
        const auto segment_offset = static_cast<std::size_t>(absolute_offset % segment_bytes);
        for (std::size_t index = 0; index < record_bytes; ++index) {
            record[index] = static_cast<std::byte>(randomish_benchmark_byte(segment_index * record_bytes + index));
        }
        const auto segment_count = std::min(segment_bytes - segment_offset, buffer.size() - offset);
        for (std::size_t copied = 0; copied < segment_count;) {
            const auto position = segment_offset + copied;
            const auto record_index = position / record_bytes;
            const auto record_offset = position % record_bytes;
            const auto count = std::min(record_bytes - record_offset, segment_count - copied);
            std::copy_n(record.begin() + static_cast<std::ptrdiff_t>(record_offset), count,
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset + copied));
            if (record_index != 0U && record_offset <= patch_offset && patch_offset - record_offset < count) {
                buffer[offset + copied + patch_offset - record_offset] ^=
                    static_cast<std::byte>(1U + (segment_index + record_index) % 255U);
            }
            copied += count;
        }
        offset += segment_count;
    }
}

// Purpose: Retain one seeded 1 MiB long-record motif without allocating it for other benchmark profiles.
// Inputs: None.
// Outputs: Returns a process-owned immutable motif for the long-sparse RAM workload.
const std::vector<std::byte>& long_sparse_record_motif() {
    static const auto record = [] {
        std::vector<std::byte> bytes(1024U * 1024U);
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(randomish_benchmark_byte(index));
        }
        return bytes;
    }();
    return record;
}

}  // namespace

// Purpose: Fill a benchmark chunk with deterministic compressed-pattern or incompressible data.
// Inputs: `buffer` is the destination, `global_offset` is its virtual file offset, `total_bytes` is the workload size,
// `profile` selects generated data shape; optional source is a stable exact-size preloaded snapshot.
// Outputs: Copies source bytes or generates benchmark bytes without filesystem access; invalid extents throw.
void fill_memory_benchmark_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset, std::uint64_t total_bytes,
                                 std::string_view profile, std::span<const std::byte> source) {
    if (!source.empty()) {
        if (source.size() != total_bytes || global_offset > total_bytes ||
            buffer.size() > total_bytes - global_offset) {
            throw ArchiveError("memory benchmark source range exceeds preloaded snapshot");
        }
        std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(global_offset), buffer.size(), buffer.begin());
        return;
    }
    if (profile == "SegmentedRecords") {
        fill_segmented_record_chunk(buffer, global_offset);
        return;
    }
    if (profile == "RepeatedRecord" || profile == "SparseRecord" || profile == "LongSparseRecord") {
        static const auto record = [] {
            std::array<std::byte, 16U * 1024U> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::byte>(randomish_benchmark_byte(index));
            }
            return bytes;
        }();
        const std::span<const std::byte> motif =
            profile == "LongSparseRecord" ? std::span(long_sparse_record_motif()) : std::span(record);
        for (std::size_t offset = 0; offset < buffer.size();) {
            const auto absolute_offset = global_offset + offset;
            const auto record_offset = static_cast<std::size_t>(absolute_offset % motif.size());
            const auto count = std::min(motif.size() - record_offset, buffer.size() - offset);
            std::copy_n(motif.begin() + static_cast<std::ptrdiff_t>(record_offset), count,
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            constexpr std::size_t patch_offset = 1024U;
            if ((profile == "SparseRecord" || profile == "LongSparseRecord") && record_offset <= patch_offset &&
                patch_offset - record_offset < count) {
                const auto record_index = absolute_offset / motif.size();
                if (profile == "SparseRecord" || record_index != 0U) {
                    buffer[offset + patch_offset - record_offset] ^= static_cast<std::byte>(1U + record_index % 255U);
                }
            }
            offset += count;
        }
        return;
    }
    std::uint64_t zero_limit = 0;
    std::uint64_t text_limit = 0;
    std::uint64_t low_entropy_limit = 0;
    if (profile == "Compressible") {
        zero_limit = total_bytes / 10U;
        text_limit = zero_limit + ((total_bytes / 10U) * 8U);
        low_entropy_limit = text_limit;
    } else if (profile == "Mixed") {
        zero_limit = total_bytes / 4U;
        text_limit = zero_limit + (total_bytes / 4U);
        low_entropy_limit = text_limit + (total_bytes / 4U);
    } else if (profile != "Incompressible") {
        throw superzip::ArchiveError("unknown memory benchmark profile: " + std::string(profile));
    }

    constexpr char text[] =
        "SuperZip memory benchmark line: AMD HIP native archive codec, metadata, and verification.\n";
    constexpr auto text_len = sizeof(text) - 1U;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const auto pos = global_offset + i;
        if (pos < zero_limit) {
            buffer[i] = std::byte{0};
        } else if (pos < text_limit) {
            buffer[i] = static_cast<std::byte>(text[pos % text_len]);
        } else if (pos < low_entropy_limit) {
            buffer[i] = static_cast<std::byte>(low_entropy_benchmark_byte(pos - text_limit));
        } else {
            buffer[i] = static_cast<std::byte>(randomish_benchmark_byte(pos));
        }
    }
}

// Purpose: Compare every decoded byte with generated or preloaded source using bounded reference storage.
// Inputs: Decoded bytes, exact offset/size/profile, reusable scratch and optional immutable source snapshot.
// Outputs: Returns after full equality, or throws ArchiveError with the first differing virtual byte offset.
void validate_memory_benchmark_bytes(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                     std::uint64_t total_bytes, std::string_view profile,
                                     std::vector<std::byte>& scratch, std::span<const std::byte> source) {
    if ((!source.empty() && source.size() != total_bytes) || global_offset > total_bytes ||
        decoded.size() > total_bytes - global_offset) {
        throw ArchiveError("memory benchmark validation range exceeds source size");
    }
    for (std::size_t offset = 0; offset < decoded.size();) {
        const auto count = std::min(kMemoryBenchmarkReferenceBytes, decoded.size() - offset);
        if (source.empty()) {
            scratch.resize(count);
            fill_memory_benchmark_chunk(scratch, global_offset + offset, total_bytes, profile);
        }
        const auto reference = source.empty() ? std::span<const std::byte>(scratch)
                                              : source.subspan(static_cast<std::size_t>(global_offset) + offset, count);
        const auto actual = decoded.subspan(offset, count);
        const auto mismatch = std::mismatch(actual.begin(), actual.end(), reference.begin());
        if (mismatch.first != actual.end()) {
            throw ArchiveError("memory benchmark byte validation mismatch at virtual offset " +
                               std::to_string(global_offset + offset + (mismatch.first - actual.begin())));
        }
        offset += count;
    }
}

// Purpose: Compare disjoint source ranges concurrently without copying or retaining decoded output.
// Inputs: Stable decoded/source storage, validated geometry, profile and admitted aggregate CPU workers.
// Outputs: Compares every byte with at most 64 KiB scratch per task; joins readers before any error escapes.
void validate_memory_benchmark_bytes_parallel(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                              std::uint64_t total_bytes, std::string_view profile,
                                              std::uint32_t workers, std::span<const std::byte> source) {
    if (workers == 0U || workers > kMaxArchiveWorkers) {
        throw ArchiveError("memory benchmark validation worker limit is outside [1, 64]");
    }
    if ((!source.empty() && source.size() != total_bytes) || global_offset > total_bytes ||
        decoded.size() > total_bytes - global_offset) {
        throw ArchiveError("memory benchmark validation range exceeds source size");
    }
    // Match the existing parallel CRC grain to avoid launching tasks for tiny byte ranges.
    constexpr std::size_t minimum_task_bytes = 8U * 1024U * 1024U;
    const auto count = std::max<std::size_t>(1U, std::min<std::size_t>(workers, decoded.size() / minimum_task_bytes));
    const auto stride = decoded.size() / count;
    const auto compare_part = [decoded, global_offset, total_bytes, profile, stride, count, source](std::size_t index) {
        const auto offset = stride * index;
        const auto extent = index + 1U == count ? decoded.size() - offset : stride;
        std::vector<std::byte> scratch;
        validate_memory_benchmark_bytes(decoded.subspan(offset, extent), global_offset + offset, total_bytes, profile,
                                        scratch, source);
    };
    std::vector<std::future<void>> pending;
    pending.reserve(count - 1U);
    for (std::size_t index = 1U; index < count; ++index) {
        pending.push_back(std::async(std::launch::async, compare_part, index));
    }
    // Future destruction joins remaining tasks on launch/caller/get failures. Ordered gets preserve the
    // earliest failing range; its serial comparison reports the first differing byte within that range.
    compare_part(0U);
    for (auto& task : pending) {
        task.get();
    }
}

}  // namespace superzip::cli
