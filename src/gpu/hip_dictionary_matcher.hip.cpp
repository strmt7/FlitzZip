#include "gpu/hip_kernel_api.hpp"
#include "gpu/hip_dictionary_optimal.hpp"
#include <rocprim/device/device_radix_sort.hpp>

namespace superzip::dictionary {
namespace {

using namespace hip_detail;

constexpr std::uint32_t kNoPrevious = 0xFFFFFFFFU;
constexpr std::uint64_t kInvalidKey = 0xFFFFFFFFFFFFFFFFULL;
static_assert(kDictionaryMaxPeriodicMatchBytes <= std::numeric_limits<std::uint16_t>::max());

using DecodeSpan = DictionarySegmentSpan;
static_assert(sizeof(DecodeSpan) == 16U);

struct DecodeSequence {
    std::uint32_t read;
    std::uint32_t written;
    std::uint32_t literal_input;
    std::uint32_t literal_output;
    std::uint32_t literals;
    std::uint32_t match_output;
    std::uint32_t distance;
    std::uint32_t match_bytes;
    std::uint32_t last_match;
    bool had_match;
    bool final;
    bool valid;
};

// Purpose: Decode a nibble-length extension without reading beyond input or exceeding the decoded budget.
// Inputs: Bounded device input, mutable read position, initial nibble, and the remaining output allowance.
// Outputs: Updates length/read and returns false on truncation or overflow; length never exceeds limit on success.
__device__ bool read_dictionary_length(const std::byte* input, std::uint32_t size, std::uint32_t& read,
                                       std::uint32_t& length, std::uint32_t limit) {
    if (length > limit) {
        return false;
    }
    if (length != 15U) {
        return true;
    }
    std::uint32_t extension = 255U;
    while (extension == 255U) {
        if (read == size) {
            return false;
        }
        extension = static_cast<std::uint32_t>(input[read++]);
        if (extension > limit - length) {
            return false;
        }
        length += extension;
    }
    return true;
}

// Purpose: Parse one complete dictionary sequence before any lane materializes its bytes.
// Inputs: Segment-local encoded data, exact decoded extent, and sequential parser state owned by lane zero.
// Outputs: Prepares bounded literal/match ranges or returns false; final sequences require exact output and LZ4 tails.
__device__ bool read_dictionary_sequence(const std::byte* input, const DecodeSpan& span, DecodeSequence& sequence) {
    if (sequence.read == span.encoded_size) {
        return false;
    }
    const auto token = static_cast<std::uint32_t>(input[sequence.read++]);
    sequence.literals = token >> 4U;
    if (!read_dictionary_length(input, span.encoded_size, sequence.read, sequence.literals,
                                span.decoded_size - sequence.written) ||
        sequence.literals > span.encoded_size - sequence.read) {
        return false;
    }
    sequence.literal_input = sequence.read;
    sequence.literal_output = sequence.written;
    sequence.read += sequence.literals;
    sequence.written += sequence.literals;
    sequence.final = sequence.read == span.encoded_size;
    if (sequence.final) {
        return sequence.written == span.decoded_size &&
               (!sequence.had_match || (sequence.literals >= 5U && span.decoded_size - sequence.last_match >= 12U));
    }
    if (span.encoded_size - sequence.read < 2U || span.decoded_size - sequence.written < 4U) {
        return false;
    }
    sequence.distance = static_cast<std::uint32_t>(input[sequence.read]) |
                        (static_cast<std::uint32_t>(input[sequence.read + 1U]) << 8U);
    sequence.read += 2U;
    sequence.match_bytes = token & 15U;
    if (sequence.distance == 0U || sequence.distance > sequence.written ||
        !read_dictionary_length(input, span.encoded_size, sequence.read, sequence.match_bytes,
                                span.decoded_size - sequence.written - 4U)) {
        return false;
    }
    sequence.match_bytes += 4U;
    sequence.match_output = sequence.written;
    sequence.last_match = sequence.written;
    sequence.had_match = true;
    sequence.written += sequence.match_bytes;
    return true;
}

// Purpose: Materialize independent LZ4-format blocks cooperatively with no inter-segment references.
// Inputs: Admitted packed device spans, encoded bytes, disjoint decoded windows, and one status per span.
// Outputs: Writes exact decoded bytes and status 1, or status 0 without publishing any partial host output.
__global__ void decode_dictionary_segments(const std::byte* encoded, const DecodeSpan* spans, std::byte* decoded,
                                           std::uint32_t* statuses) {
    const auto index = static_cast<std::uint32_t>(blockIdx.x);
    const auto span = spans[index];
    const auto* input = encoded + span.encoded_offset;
    auto* output = decoded + span.decoded_offset;
    __shared__ DecodeSequence sequence;
    if (threadIdx.x == 0U) {
        sequence = DecodeSequence{};
    }
    __syncthreads();
    while (true) {
        if (threadIdx.x == 0U) {
            sequence.valid = read_dictionary_sequence(input, span, sequence);
        }
        __syncthreads();
        if (!sequence.valid) {
            if (threadIdx.x == 0U) {
                statuses[index] = 0U;
            }
            return;
        }
        for (auto byte = static_cast<std::uint32_t>(threadIdx.x); byte < sequence.literals; byte += blockDim.x) {
            output[sequence.literal_output + byte] = input[sequence.literal_input + byte];
        }
        __syncthreads();
        if (sequence.final) {
            if (threadIdx.x == 0U) {
                statuses[index] = 1U;
            }
            return;
        }
        // Overlapping LZ4 copies repeat the already materialized distance-byte period, not new peer writes.
        for (auto byte = static_cast<std::uint32_t>(threadIdx.x); byte < sequence.match_bytes; byte += blockDim.x) {
            output[sequence.match_output + byte] =
                output[sequence.match_output - sequence.distance + byte % sequence.distance];
        }
        __syncthreads();
    }
}

// Purpose: Produce exact-prefix keys ordered by segment, four-byte value, and source position.
// Inputs: Immutable device input and its bounded byte count; keys has one element per input byte.
// Outputs: Writes unique sortable keys, or a sentinel where four readable bytes do not remain in the segment.
__global__ void build_dictionary_keys(const std::byte* input, std::uint32_t size, std::uint64_t* keys) {
    const auto position = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (position >= size) {
        return;
    }
    const auto segment = position / kSegmentBytes;
    const auto end = min(size, (segment + 1U) * kSegmentBytes);
    if (end - position < kMinMatchBytes) {
        keys[position] = kInvalidKey;
        return;
    }
    std::uint32_t word = 0;
    for (std::uint32_t byte = 0; byte < kMinMatchBytes; ++byte) {
        word |= static_cast<std::uint32_t>(input[position + byte]) << (byte * 8U);
    }
    keys[position] = (static_cast<std::uint64_t>(segment) << 48U) | (static_cast<std::uint64_t>(word) << 16U) |
                     (position % kSegmentBytes);
}

// Purpose: Link each exact four-byte prefix to its nearest earlier occurrence within the same segment.
// Inputs: Ascending unique valid keys followed by optional sentinels, and a source-position-indexed output table.
// Outputs: Writes acyclic, strictly backward links; invalid final-byte positions are never read by the search.
__global__ void link_dictionary_predecessors(const std::uint64_t* keys, std::uint32_t size, std::uint32_t* previous) {
    const auto index = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= size || keys[index] == kInvalidKey) {
        return;
    }
    const auto key = keys[index];
    const auto segment_start = static_cast<std::uint32_t>(key >> 48U) * kSegmentBytes;
    const auto position = segment_start + static_cast<std::uint32_t>(key & 0xFFFFU);
    previous[position] = index > 0U && (keys[index - 1U] >> 16U) == (key >> 16U)
                             ? segment_start + static_cast<std::uint32_t>(keys[index - 1U] & 0xFFFFU)
                             : kNoPrevious;
}

// Purpose: Resolve an exact predecessor from either the sorted index or a sampled periodic distance.
// Inputs: Compile-time index policy, immutable input, valid source position, and its selected index data.
// Outputs: Returns a verified earlier position or the no-match sentinel without crossing segment bounds.
template <bool Periodic>
__device__ std::uint32_t predecessor_at(const std::byte* input, std::uint32_t size, std::uint32_t position,
                                        const std::uint32_t* previous, const std::uint16_t* distances) {
    if constexpr (!Periodic) {
        return previous[position];
    } else {
        const auto segment = position / kSegmentBytes;
        const auto segment_offset = position % kSegmentBytes;
        const auto distance = distances[segment];
        const auto end = min(size, (segment + 1U) * kSegmentBytes);
        if (segment_offset < distance || end - position < kMinMatchBytes) {
            return kNoPrevious;
        }
        std::uint32_t current = 0U;
        std::uint32_t candidate = 0U;
        __builtin_memcpy(&current, input + position, sizeof(current));
        __builtin_memcpy(&candidate, input + position - distance, sizeof(candidate));
        return current == candidate ? position - distance : kNoPrevious;
    }
}

// Purpose: Extend a known four-byte match with alignment-safe word reads and a bounded final byte tail.
// Inputs: Immutable input, source positions, length limit, effort, and mutable comparison accounting.
// Outputs: Returns only verified equal bytes; charges all bytes loaded for comparisons against the work budget.
__device__ std::uint32_t extend_dictionary_match(const std::byte* input, std::uint32_t position,
                                                 std::uint32_t candidate, std::uint32_t limit, Effort effort,
                                                 Match& accounting) {
    std::uint32_t length = kMinMatchBytes;
    if (effort.max_candidates == 1U) {
        while (limit - length >= 8U && effort.max_byte_comparisons - accounting.bytes_compared >= 8U) {
            std::uint64_t left = 0;
            std::uint64_t right = 0;
            __builtin_memcpy(&left, input + position + length, sizeof(left));
            __builtin_memcpy(&right, input + candidate + length, sizeof(right));
            accounting.bytes_compared += 8U;
            const auto difference = left ^ right;
            if (difference != 0U) {
                // AMD HIP's little-endian word order puts the earliest mismatching byte first.
                return length + static_cast<std::uint32_t>(__builtin_ctzll(difference) / 8U);
            }
            length += 8U;
        }
    }
    while (limit - length >= 4U && effort.max_byte_comparisons - accounting.bytes_compared >= 4U) {
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        __builtin_memcpy(&left, input + position + length, sizeof(left));
        __builtin_memcpy(&right, input + candidate + length, sizeof(right));
        accounting.bytes_compared += 4U;
        auto difference = left ^ right;
        if (difference != 0U) {
            // AMD HIP byte order places the earliest differing byte in the least-significant nonzero group.
            while ((difference & 0xFFU) == 0U) {
                ++length;
                difference >>= 8U;
            }
            return length;
        }
        length += 4U;
    }
    while (length < limit && accounting.bytes_compared < effort.max_byte_comparisons) {
        ++accounting.bytes_compared;
        if (input[candidate + length] != input[position + length]) {
            break;
        }
        ++length;
    }
    return length;
}

template <bool Periodic, bool Neutron = false>
// Purpose: Search one deterministic predecessor chain with explicit per-position work budgets.
// Inputs: Immutable source, valid position, selected index data, and bounded effort.
// Outputs: Returns a verified match up to 8 KiB normally, 32 KiB for periodic work, or 65,535 bytes for Neutron.
__device__ Match search_dictionary_position(const std::byte* input, std::uint32_t size, const std::uint32_t* previous,
                                            const std::uint16_t* distances, Effort effort, std::uint32_t position) {
    Match best{};
    const auto segment_start = (position / kSegmentBytes) * kSegmentBytes;
    const auto end = min(size, segment_start + kSegmentBytes);
    if (end - position < kMinMatchBytes) {
        return best;
    }
    const auto limit =
        min(Neutron ? kMaxNeutronMatchBytes : (Periodic ? kDictionaryMaxPeriodicMatchBytes : kMaxMatchBytes),
            end - position);
    auto candidate = predecessor_at<Periodic>(input, size, position, previous, distances);
    while (candidate < position && candidate >= segment_start && best.length < limit &&
           best.candidates_examined < effort.max_candidates && best.bytes_compared < effort.max_byte_comparisons) {
        ++best.candidates_examined;
        bool can_improve = true;
        if (best.length >= kMinMatchBytes) {
            ++best.bytes_compared;
            can_improve = input[candidate + best.length] == input[position + best.length];
        }
        if (can_improve) {
            const auto length = extend_dictionary_match(input, position, candidate, limit, effort, best);
            if (length > best.length) {
                best.distance = static_cast<std::uint16_t>(position - candidate);
                best.length = static_cast<std::uint16_t>(length);
            }
        }
        candidate = predecessor_at<Periodic>(input, size, candidate, previous, distances);
    }
    return best;
}

// Purpose: Materialize diagnostic matches for every source position using the shared bounded search.
// Inputs: Immutable source, ordered links, bounded effort, and one output record per byte.
// Outputs: Writes exact match records without affecting the demand-driven encoding path.
__global__ void search_dictionary_matches(const std::byte* input, std::uint32_t size, const std::uint32_t* previous,
                                          Effort effort, Match* matches) {
    const auto position = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (position < size) {
        matches[position] = search_dictionary_position<false>(input, size, previous, nullptr, effort, position);
    }
}

// Purpose: Materialize deeper verified matches in a bounded launch rather than one unbounded whole-input search.
// Inputs: Exact predecessor links, admitted source and a tile of at most 4096 positions.
// Outputs: Writes one verified match per tile byte; sixteen-bit work counters cannot overflow their fixed budgets.
__global__ void search_neutron_matches(const std::byte* input, std::uint32_t size, const std::uint32_t* previous,
                                       std::uint32_t first, std::uint32_t count, Match* matches) {
    const auto offset = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (offset < count) {
        matches[first + offset] = search_dictionary_position<false, true>(
            input, size, previous, nullptr, {.max_candidates = 1024U, .max_byte_comparisons = 65535U}, first + offset);
    }
}

// Purpose: Count extension bytes for an LZ4 nibble-encoded length.
// Inputs: A bounded literal count or match length minus four.
// Outputs: Returns zero below fifteen, otherwise includes the required terminal extension byte.
__device__ std::uint32_t length_extension_bytes(std::uint32_t length) {
    return length < 15U ? 0U : (length - 15U) / 255U + 1U;
}

// Purpose: Write an LZ4 length extension after its token or offset.
// Inputs: A bounded length and sufficient caller-checked output capacity.
// Outputs: Writes extension bytes and returns the next output position.
__device__ std::uint32_t write_length_extension(std::byte* output, std::uint32_t position, std::uint32_t length) {
    if (length >= 15U) {
        length -= 15U;
        while (length >= 255U) {
            output[position++] = std::byte{255};
            length -= 255U;
        }
        output[position++] = static_cast<std::byte>(length);
    }
    return position;
}

// Purpose: Emit one bounded LZ4 sequence header and reserve its literal span.
// Inputs: Selected match, literal extent, destination capacity and mutable segment cursors.
// Outputs: Returns false before any write on capacity failure; otherwise advances only bounded cursors.
__device__ bool write_dictionary_sequence(std::byte* destination, std::uint32_t literal_bytes, bool last,
                                          std::uint32_t match_length, std::uint16_t match_distance,
                                          std::uint32_t& written, std::uint32_t& literal_output) {
    const auto match_code = last ? 0U : match_length - kMinMatchBytes;
    const auto needed = 1U + length_extension_bytes(literal_bytes) + literal_bytes +
                        (last ? 0U : 2U + length_extension_bytes(match_code));
    if (written > kEncodedSegmentCapacity || needed > kEncodedSegmentCapacity - written) {
        return false;
    }
    destination[written++] = static_cast<std::byte>((min(literal_bytes, 15U) << 4U) | min(match_code, 15U));
    literal_output = write_length_extension(destination, written, literal_bytes);
    written = literal_output + literal_bytes;
    if (!last) {
        destination[written++] = static_cast<std::byte>(match_distance & 0xFFU);
        destination[written++] = static_cast<std::byte>(match_distance >> 8U);
        written = write_length_extension(destination, written, match_code);
    }
    return true;
}

// Purpose: Pack LZ4 blocks with eager cached matches or deferred selected-position search.
// Inputs: Compile-time cache/index policies, source, index data, effort, output slots, and segment sizes.
// Outputs: Writes complete blocks, or a zero size if a capacity invariant fails; no output crosses its slot.
template <bool CacheMatches, bool Periodic>
// Purpose: Pack bounded LZ4 segments. Inputs: Verified source/index. Outputs: Complete capacity-checked blocks.
// Invariant: Every lane reaches barriers together; the selected sequence never crosses its output slot.
__global__ void encode_dictionary_segments(const std::byte* input, std::uint32_t size, const std::uint32_t* previous,
                                           const std::uint16_t* distances, Effort effort, std::byte* output,
                                           std::uint32_t* encoded_sizes) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    const auto start = segment * kSegmentBytes;
    const auto end = min(size, start + kSegmentBytes);
    auto* destination = output + segment * kEncodedSegmentCapacity;
    __shared__ std::uint32_t cursor;
    __shared__ std::uint32_t next_match;
    __shared__ std::uint32_t written;
    __shared__ std::uint32_t literal_output;
    __shared__ std::uint32_t match_length;
    __shared__ std::uint16_t match_distance;
    __shared__ std::uint32_t cache_start;
    __shared__ std::uint32_t cache_end;
    __shared__ bool cached_has_match[kDictionaryKernelThreads];
    __shared__ std::uint16_t cached_lengths[kDictionaryKernelThreads];
    __shared__ std::uint16_t cached_distances[kDictionaryKernelThreads];
    __shared__ bool failed;
    // Lane zero publishes shared cursors before any lane enters the cooperative search.
    if (threadIdx.x == 0U) {
        cursor = start;
        written = 0;
        cache_start = start;
        cache_end = start;
        failed = false;
    }
    __syncthreads();
    while (true) {
        if (threadIdx.x == 0U) {
            next_match = end;
        }
        __syncthreads();
        // Low efforts reuse complete matches; high efforts defer extension until the selected position.
        for (auto tile = cursor; tile < end && end - tile >= 12U; tile = cache_end) {
            const bool refill = tile >= cache_end;
            __syncthreads();
            if (refill) {
                if (threadIdx.x == 0U) {
                    cache_start = tile;
                    cache_end = min(end, tile + kDictionaryKernelThreads);
                }
                __syncthreads();
                const auto position = cache_start + static_cast<std::uint32_t>(threadIdx.x);
                if constexpr (CacheMatches) {
                    const auto match =
                        position < end && end - position >= 12U
                            ? search_dictionary_position<Periodic>(input, size, previous, distances, effort, position)
                            : Match{};
                    cached_lengths[threadIdx.x] = match.length;
                    cached_distances[threadIdx.x] = match.distance;
                    cached_has_match[threadIdx.x] = match.length >= kMinMatchBytes;
                } else {
                    cached_has_match[threadIdx.x] =
                        position < end && end - position >= 12U &&
                        predecessor_at<Periodic>(input, size, position, previous, distances) != kNoPrevious;
                }
                __syncthreads();
            }
            const auto position = cache_start + static_cast<std::uint32_t>(threadIdx.x);
            if (position >= cursor && cached_has_match[threadIdx.x]) {
                atomicMin(&next_match, position);
            }
            __syncthreads();
            if (next_match != end) {
                break;
            }
        }
        __syncthreads();
        // All lanes observe the same selected match before sequence pricing or literal copies.
        const auto literal_bytes = next_match - cursor;
        const bool last = next_match == end;
        if (threadIdx.x == 0U) {
            Match match{};
            if (!last) {
                if constexpr (CacheMatches) {
                    const auto index = next_match - cache_start;
                    match.length = cached_lengths[index];
                    match.distance = cached_distances[index];
                } else {
                    match = search_dictionary_position<Periodic>(input, size, previous, distances, effort, next_match);
                }
            }
            match_length = last ? 0U : min(static_cast<std::uint32_t>(match.length), end - next_match - 5U);
            match_distance = match.distance;
            failed = !write_dictionary_sequence(destination, literal_bytes, last, match_length, match_distance, written,
                                                literal_output);
        }
        __syncthreads();
        if (failed) {
            if (threadIdx.x == 0U) {
                encoded_sizes[segment] = 0U;
            }
            return;
        }
        for (auto index = static_cast<std::uint32_t>(threadIdx.x); index < literal_bytes;
             index += kDictionaryKernelThreads) {
            destination[literal_output + index] = input[cursor + index];
        }
        __syncthreads();
        // Publish the completed size only after all literal writes have reached the barrier.
        if (last) {
            if (threadIdx.x == 0U) {
                encoded_sizes[segment] = written;
            }
            return;
        }
        if (threadIdx.x == 0U) {
            cursor = next_match + match_length;
        }
        __syncthreads();
    }
}

// Purpose: Gather bounded LZ4 segment payloads into one contiguous device transfer.
// Inputs: Fixed-capacity encoded slots and validated per-segment lengths; `packed` has room for their sum.
// Outputs: Writes each segment in archive order without changing any encoded bytes.
__global__ void compact_dictionary_segments(const std::byte* encoded, const std::uint32_t* encoded_sizes,
                                            std::uint32_t segment_count, std::byte* packed) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    std::uint32_t packed_offset = 0U;
    for (std::uint32_t prior = 0; prior < segment; ++prior) {
        packed_offset += encoded_sizes[prior];
    }
    const auto slot_offset = static_cast<std::size_t>(segment) * kEncodedSegmentCapacity;
    for (std::uint32_t index = threadIdx.x; index < encoded_sizes[segment]; index += blockDim.x) {
        packed[packed_offset + index] = encoded[slot_offset + index];
    }
}

}  // namespace

// Purpose: Expose the existing radix primitive without transferring an allocator, container or exception.
// Inputs: Caller-owned device/scratch buffers and an in/out scratch extent on the admitted per-thread stream.
// Outputs: Returns the HIP status and required scratch bytes; retains no caller storage.
hipError_t sort_dictionary_keys(void* scratch, std::size_t* bytes, const std::uint64_t* input, std::uint64_t* output,
                                std::uint32_t count, hipStream_t stream) noexcept {
    return rocprim::radix_sort_keys(scratch, *bytes, input, output, count, 0, 64, stream);
}

// Purpose: Bind this translation unit's registered kernels to the private POD dispatch table.
// Inputs: Module-owned table under construction after DLL registration.
// Outputs: Writes only this component's typed entrypoints; allocates no storage.
void bind_dictionary_kernels(HipKernelApi& api) noexcept {
    api.decode_dictionary_segments = decode_dictionary_segments;
    api.build_dictionary_keys = build_dictionary_keys;
    api.link_dictionary_predecessors = link_dictionary_predecessors;
    api.search_dictionary_matches = search_dictionary_matches;
    api.search_neutron_matches = search_neutron_matches;
    api.encode_dictionary_deferred = encode_dictionary_segments<false, false>;
    api.encode_dictionary_cached = encode_dictionary_segments<true, false>;
    api.encode_dictionary_periodic = encode_dictionary_segments<false, true>;
    api.compact_dictionary_segments = compact_dictionary_segments;
    api.neutron_bound_match_graphs = optimal::bound_match_graphs;
    api.neutron_initialize = optimal::initialize;
    api.neutron_classify_match_graphs = optimal::classify_match_graphs;
    api.neutron_initialize_noncompetitive_graphs = optimal::initialize_noncompetitive_graphs;
    api.neutron_parse_tile = optimal::parse_tile;
    api.neutron_initialize_writers = optimal::initialize_writers;
    api.neutron_emit_tile = optimal::emit_tile;
    api.sort_dictionary_keys = sort_dictionary_keys;
}

}  // namespace superzip::dictionary
