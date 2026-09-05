#include "gpu/dictionary_matcher.hpp"
#include "gpu/hip_codec_support.hpp"

#include <algorithm>
#include <limits>
#include <rocprim/device/device_radix_sort.hpp>
#include <rocprim/rocprim_version.hpp>

namespace superzip::dictionary {
namespace {

using namespace hip_detail;
constexpr std::uint32_t kThreads = 256U;
constexpr std::uint32_t kNoPrevious = 0xFFFFFFFFU;
constexpr std::uint64_t kInvalidKey = 0xFFFFFFFFFFFFFFFFULL;

struct DecodeSpan {
    std::uint32_t encoded_offset;
    std::uint32_t encoded_size;
    std::uint32_t decoded_offset;
    std::uint32_t decoded_size;
};
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

// Purpose: Extend a known four-byte match with alignment-safe word reads and a bounded final byte tail.
// Inputs: Immutable input, source positions, length limit, effort, and mutable comparison accounting.
// Outputs: Returns only verified equal bytes; charges all bytes loaded for comparisons against the work budget.
__device__ std::uint32_t extend_dictionary_match(const std::byte* input, std::uint32_t position,
                                                 std::uint32_t candidate, std::uint32_t limit, Effort effort,
                                                 Match& accounting) {
    std::uint32_t length = kMinMatchBytes;
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

// Purpose: Search deterministic predecessor chains with explicit per-position work budgets.
// Inputs: Immutable source bytes, ordered predecessor links, bounded effort, and one output record per byte.
// Outputs: Writes verified matches of 4..8192 bytes, preserving nearest references on equal length and accounting work.
__global__ void search_dictionary_matches(const std::byte* input, std::uint32_t size, const std::uint32_t* previous,
                                          Effort effort, Match* matches) {
    const auto position = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (position >= size) {
        return;
    }
    Match best{};
    const auto segment_start = (position / kSegmentBytes) * kSegmentBytes;
    const auto end = min(size, segment_start + kSegmentBytes);
    if (end - position < kMinMatchBytes) {
        matches[position] = best;
        return;
    }
    const auto limit = min(kMaxMatchBytes, end - position);
    auto candidate = previous[position];
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
        candidate = previous[candidate];
    }
    matches[position] = best;
}

// Purpose: Finish a multi-kernel stage without misreporting a library operation as a single kernel launch.
// Inputs: Recorded HIP events surrounding ordered work in the per-thread stream.
// Outputs: Returns valid elapsed milliseconds or nullopt; synchronizes and throws on asynchronous HIP errors.
std::optional<double> finish_dictionary_stage(const HipEventPair& events) {
    check_hip(hipEventSynchronize(events.stop), "synchronize dictionary stage");
    float milliseconds = 0;
    check_hip(hipEventElapsedTime(&milliseconds, events.start, events.stop), "measure dictionary stage");
    return validated_stage_milliseconds(static_cast<double>(milliseconds));
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

// Purpose: Select and pack independent LZ4-format blocks with cooperative literal search and copying.
// Inputs: Original bytes and verified GPU matches; fixed-capacity output slots and one size per segment.
// Outputs: Writes complete blocks, or a zero size if a capacity invariant fails; no output crosses its slot.
__global__ void encode_dictionary_segments(const std::byte* input, std::uint32_t size, const Match* matches,
                                           std::byte* output, std::uint32_t* encoded_sizes) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    const auto start = segment * kSegmentBytes;
    const auto end = min(size, start + kSegmentBytes);
    auto* destination = output + segment * kEncodedSegmentCapacity;
    __shared__ std::uint32_t cursor;
    __shared__ std::uint32_t next_match;
    __shared__ std::uint32_t written;
    __shared__ std::uint32_t literal_output;
    __shared__ std::uint32_t match_length;
    __shared__ bool failed;
    if (threadIdx.x == 0U) {
        cursor = start;
        written = 0;
        failed = false;
    }
    __syncthreads();
    while (true) {
        if (threadIdx.x == 0U) {
            next_match = end;
        }
        __syncthreads();
        // Stop all lanes at the first useful tile instead of rescanning the remaining suffix in empty lanes.
        for (auto tile = cursor; tile < end && end - tile >= 12U; tile += kThreads) {
            const auto position = tile + static_cast<std::uint32_t>(threadIdx.x);
            if (position < end && end - position >= 12U && matches[position].length >= kMinMatchBytes) {
                atomicMin(&next_match, position);
            }
            __syncthreads();
            if (next_match != end) {
                break;
            }
        }
        __syncthreads();
        const auto literal_bytes = next_match - cursor;
        const bool last = next_match == end;
        if (threadIdx.x == 0U) {
            match_length =
                last ? 0U : min(static_cast<std::uint32_t>(matches[next_match].length), end - next_match - 5U);
            const auto match_code = last ? 0U : match_length - kMinMatchBytes;
            const auto needed = 1U + length_extension_bytes(literal_bytes) + literal_bytes +
                                (last ? 0U : 2U + length_extension_bytes(match_code));
            failed = needed > kEncodedSegmentCapacity - written;
            if (!failed) {
                destination[written++] = static_cast<std::byte>((min(literal_bytes, 15U) << 4U) | min(match_code, 15U));
                literal_output = write_length_extension(destination, written, literal_bytes);
                written = literal_output + literal_bytes;
                if (!last) {
                    const auto distance = matches[next_match].distance;
                    destination[written++] = static_cast<std::byte>(distance & 0xFFU);
                    destination[written++] = static_cast<std::byte>(distance >> 8U);
                    written = write_length_extension(destination, written, match_code);
                }
            }
        }
        __syncthreads();
        if (failed) {
            if (threadIdx.x == 0U) {
                encoded_sizes[segment] = 0U;
            }
            return;
        }
        for (auto index = static_cast<std::uint32_t>(threadIdx.x); index < literal_bytes; index += kThreads) {
            destination[literal_output + index] = input[cursor + index];
        }
        __syncthreads();
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

// Purpose: Share bounded GPU indexing/search while keeping matches resident for the chosen consumer.
// Inputs: Validated input/effort, additional consumer workspace, and a synchronous consumer of borrowed device buffers.
// Outputs: Returns the consumer's result; owns and releases all index/search allocations, including on failure.
template <typename Consumer>
auto with_dictionary_matches(std::span<const std::byte> input, const Effort& effort, std::size_t consumer_bytes,
                             Consumer consume) {
    const auto size = static_cast<std::uint32_t>(input.size());
    const auto blocks = (size + kThreads - 1U) / kThreads;
    const auto key_bytes = checked_multiply_bytes(input.size(), sizeof(std::uint64_t), "dictionary keys");
    const auto previous_bytes = checked_multiply_bytes(input.size(), sizeof(std::uint32_t), "dictionary links");
    const auto match_bytes = checked_multiply_bytes(input.size(), sizeof(Match), "dictionary matches");
    std::size_t temporary_bytes = 0;
    check_hip(rocprim::radix_sort_keys(nullptr, temporary_bytes, static_cast<std::uint64_t*>(nullptr),
                                       static_cast<std::uint64_t*>(nullptr), size, 0, 64, hipStreamPerThread),
              "query dictionary radix-sort workspace");
    const auto scratch_bytes = std::max<std::size_t>(temporary_bytes, 1U);
    auto required_bytes = checked_add_bytes(input.size(), key_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, key_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, previous_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, match_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, scratch_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, consumer_bytes, "dictionary consumer workspace");
    if (required_bytes > kMaxWorkspaceBytes) {
        throw GpuError("dictionary workspace exceeds the per-batch GPU memory limit");
    }
    HipDeviceMemoryReservation reservation(required_bytes, "dictionary match finder");
    HipDeviceBuffer<std::byte> device_input(input.size(), "allocate dictionary input");
    HipDeviceBuffer<std::uint64_t> keys(key_bytes, "allocate dictionary keys");
    HipDeviceBuffer<std::uint64_t> sorted_keys(key_bytes, "allocate sorted dictionary keys");
    HipDeviceBuffer<std::uint32_t> previous(previous_bytes, "allocate dictionary links");
    HipDeviceBuffer<Match> matches(match_bytes, "allocate dictionary matches");
    HipDeviceBuffer<std::byte> temporary(scratch_bytes, "allocate dictionary radix workspace");
    check_hip(hipMemcpy(device_input.get(), input.data(), input.size(), hipMemcpyHostToDevice),
              "upload dictionary input");
    auto events = make_hip_event_pair("create dictionary timing events");
    check_hip(hipEventRecord(events.start, hipStreamPerThread), "start dictionary index");
    build_dictionary_keys<<<blocks, kThreads, 0, hipStreamPerThread>>>(device_input.get(), size, keys.get());
    check_hip(hipGetLastError(), "launch dictionary keys");
    check_hip(rocprim::radix_sort_keys(temporary.get(), temporary_bytes, keys.get(), sorted_keys.get(), size, 0, 64,
                                       hipStreamPerThread),
              "sort dictionary keys");
    link_dictionary_predecessors<<<blocks, kThreads, 0, hipStreamPerThread>>>(sorted_keys.get(), size, previous.get());
    check_hip(hipGetLastError(), "launch dictionary links");
    check_hip(hipEventRecord(events.stop, hipStreamPerThread), "stop dictionary index");
    MatchBatch result;
    result.index_ms = finish_dictionary_stage(events);
    check_hip(hipEventRecord(events.start, hipStreamPerThread), "start dictionary search");
    search_dictionary_matches<<<blocks, kThreads, 0, hipStreamPerThread>>>(device_input.get(), size, previous.get(),
                                                                           effort, matches.get());
    check_hip(hipGetLastError(), "launch dictionary search");
    check_hip(hipEventRecord(events.stop, hipStreamPerThread), "stop dictionary search");
    result.search_ms = finish_dictionary_stage(events);
    result.device_workspace_bytes = required_bytes;
    result.h2d_bytes = input.size();
    result.primitive_version = ROCPRIM_VERSION;
    result.gpu_used = true;
    auto consumed = consume(device_input.get(), matches.get(), result);
    temporary.reset_checked("free dictionary radix workspace");
    matches.reset_checked("free dictionary matches");
    previous.reset_checked("free dictionary links");
    sorted_keys.reset_checked("free sorted dictionary keys");
    keys.reset_checked("free dictionary keys");
    device_input.reset_checked("free dictionary input");
    return consumed;
}

}  // namespace

// Purpose: Download diagnostic matches from the shared HIP dictionary search.
// Inputs: A validated immutable batch and bounded effort.
// Outputs: Returns exact match records and resource telemetry; no encoded payload is produced.
MatchBatch find_matches_hip(std::span<const std::byte> input, const Effort& effort) {
    return with_dictionary_matches(
        input, effort, 0U, [input](const std::byte*, const Match* device_matches, MatchBatch metadata) {
            metadata.matches.resize(input.size());
            metadata.d2h_bytes = input.size() * sizeof(Match);
            check_hip(hipMemcpy(metadata.matches.data(), device_matches, metadata.d2h_bytes, hipMemcpyDeviceToHost),
                      "download dictionary matches");
            return metadata;
        });
}

// Purpose: Pack GPU-resident matches into independent blocks and download only used bytes plus bounded sizes.
// Inputs: Validated nonempty source and effort; output slots are bounded by the LZ4 compression bound.
// Outputs: Returns encoded segments and transfer/workspace counters, or throws before exposing incomplete output.
EncodedBatch encode_segments_hip(std::span<const std::byte> input, const Effort& effort) {
    const auto segment_count = (input.size() + kSegmentBytes - 1U) / kSegmentBytes;
    const auto output_bytes = segment_count * kEncodedSegmentCapacity;
    const auto sizes_bytes = segment_count * sizeof(std::uint32_t);
    return with_dictionary_matches(
        input, effort, output_bytes + sizes_bytes,
        [=](const std::byte* device_input, const Match* device_matches, const MatchBatch& metadata) {
            HipDeviceBuffer<std::byte> output(output_bytes, "allocate dictionary encoded slots");
            HipDeviceBuffer<std::uint32_t> sizes(sizes_bytes, "allocate dictionary encoded sizes");
            encode_dictionary_segments<<<static_cast<unsigned int>(segment_count), kThreads, 0, hipStreamPerThread>>>(
                device_input, static_cast<std::uint32_t>(input.size()), device_matches, output.get(), sizes.get());
            check_hip(hipGetLastError(), "launch dictionary segment encoder");
            check_hip(hipStreamSynchronize(hipStreamPerThread), "synchronize dictionary segment encoder");
            std::vector<std::uint32_t> host_sizes(segment_count);
            check_hip(hipMemcpy(host_sizes.data(), sizes.get(), sizes_bytes, hipMemcpyDeviceToHost),
                      "download dictionary encoded sizes");
            EncodedBatch result;
            result.device_workspace_bytes = metadata.device_workspace_bytes;
            result.h2d_bytes = metadata.h2d_bytes;
            result.d2h_bytes = sizes_bytes;
            result.gpu_used = true;
            for (std::size_t index = 0; index < segment_count; ++index) {
                if (host_sizes[index] == 0U || host_sizes[index] > kEncodedSegmentCapacity) {
                    throw GpuError("dictionary encoder exceeded its segment capacity");
                }
                EncodedSegment segment;
                segment.input_bytes = static_cast<std::uint32_t>(
                    std::min(input.size() - index * kSegmentBytes, std::size_t{kSegmentBytes}));
                segment.payload.resize(host_sizes[index]);
                check_hip(hipMemcpy(segment.payload.data(), output.get() + index * kEncodedSegmentCapacity,
                                    segment.payload.size(), hipMemcpyDeviceToHost),
                          "download dictionary encoded block");
                result.d2h_bytes += segment.payload.size();
                result.segments.push_back(std::move(segment));
            }
            sizes.reset_checked("free dictionary encoded sizes");
            output.reset_checked("free dictionary encoded slots");
            return result;
        });
}

// Purpose: Upload bounded dictionary spans once and return bytes only after every GPU decoder reports success.
// Inputs: Host-admitted nonempty segments and their summed decoded extent, at most 4 MiB.
// Outputs: Returns exact bytes and resource/timing evidence; releases all HIP resources on success or failure.
DecodedBatch decode_segments_hip(std::span<const EncodedSegment> segments, std::size_t decoded_bytes) {
    std::vector<DecodeSpan> spans;
    std::vector<std::byte> encoded;
    std::size_t encoded_bytes = 0;
    for (const auto& segment : segments) {
        encoded_bytes += segment.payload.size();
    }
    spans.reserve(segments.size());
    encoded.reserve(encoded_bytes);
    std::size_t decoded_offset = 0;
    for (const auto& segment : segments) {
        spans.push_back({static_cast<std::uint32_t>(encoded.size()), static_cast<std::uint32_t>(segment.payload.size()),
                         static_cast<std::uint32_t>(decoded_offset), segment.input_bytes});
        encoded.insert(encoded.end(), segment.payload.begin(), segment.payload.end());
        decoded_offset += segment.input_bytes;
    }
    const auto span_bytes = spans.size() * sizeof(DecodeSpan);
    const auto status_bytes = spans.size() * sizeof(std::uint32_t);
    const auto required_bytes = encoded.size() + decoded_bytes + span_bytes + status_bytes;
    HipDeviceMemoryReservation reservation(required_bytes, "dictionary decoder");
    HipDeviceBuffer<std::byte> input(encoded.size(), "allocate dictionary decode input");
    HipDeviceBuffer<DecodeSpan> device_spans(span_bytes, "allocate dictionary decode spans");
    HipDeviceBuffer<std::byte> output(decoded_bytes, "allocate dictionary decode output");
    HipDeviceBuffer<std::uint32_t> statuses(status_bytes, "allocate dictionary decode statuses");
    check_hip(hipMemcpy(input.get(), encoded.data(), encoded.size(), hipMemcpyHostToDevice),
              "upload dictionary blocks");
    check_hip(hipMemcpy(device_spans.get(), spans.data(), span_bytes, hipMemcpyHostToDevice),
              "upload dictionary spans");
    auto events = make_hip_event_pair("create dictionary decode events");
    check_hip(hipEventRecord(events.start, hipStreamPerThread), "start dictionary decode");
    decode_dictionary_segments<<<static_cast<unsigned int>(spans.size()), kThreads, 0, hipStreamPerThread>>>(
        input.get(), device_spans.get(), output.get(), statuses.get());
    check_hip(hipGetLastError(), "launch dictionary decoder");
    check_hip(hipEventRecord(events.stop, hipStreamPerThread), "stop dictionary decode");
    DecodedBatch result;
    result.decode_ms = finish_dictionary_stage(events);
    std::vector<std::uint32_t> host_statuses(spans.size());
    check_hip(hipMemcpy(host_statuses.data(), statuses.get(), status_bytes, hipMemcpyDeviceToHost),
              "download dictionary decode statuses");
    if (std::any_of(host_statuses.begin(), host_statuses.end(), [](auto status) { return status != 1U; })) {
        throw ArchiveError("dictionary block failed GPU decoding validation");
    }
    result.bytes.resize(decoded_bytes);
    check_hip(hipMemcpy(result.bytes.data(), output.get(), decoded_bytes, hipMemcpyDeviceToHost),
              "download dictionary decoded bytes");
    result.device_workspace_bytes = required_bytes;
    result.h2d_bytes = encoded.size() + span_bytes;
    result.d2h_bytes = decoded_bytes + status_bytes;
    result.gpu_used = true;
    statuses.reset_checked("free dictionary decode statuses");
    output.reset_checked("free dictionary decode output");
    device_spans.reset_checked("free dictionary decode spans");
    input.reset_checked("free dictionary decode input");
    return result;
}

}  // namespace superzip::dictionary
