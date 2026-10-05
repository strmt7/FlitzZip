#pragma once

#include "gpu/dictionary_matcher.hpp"
#include "gpu/hip_codec_support.hpp"

#include <cstdint>
#include <limits>

namespace superzip::dictionary::optimal {

inline constexpr std::uint32_t kLanes = 256U;
inline constexpr std::uint32_t kTreeWords = 2U * kSegmentBytes;
inline constexpr std::uint32_t kResidueTreeWords = 512U;
inline constexpr std::uint32_t kPositionsPerLaunch = kNeutronParseTilePositions;
inline constexpr std::uint32_t kSearchPositionsPerLaunch = kNeutronSearchTileBytes;
inline constexpr std::uint32_t kSequencesPerLaunch = kNeutronEmitSequences;
inline constexpr auto kInfinity = std::numeric_limits<std::uint64_t>::max();

struct State {
    Match* matches;
    std::uint64_t* suffix_tree;
    std::uint64_t* residue_tree;
    std::uint32_t* match_costs;
    std::uint32_t* match_ends;
    std::uint32_t* next_matches;
};

struct EmitCursor {
    std::uint32_t position;
    std::uint32_t written;
};

// Purpose: Preserve deterministic minimum-cost ties with one ordered integer.
// Inputs: Bounded byte cost and segment-local source position.
// Outputs: Returns a cost-first, position-second key; valid keys are smaller than the infinity sentinel.
__device__ std::uint64_t key(std::uint32_t cost, std::uint32_t position) {
    return (static_cast<std::uint64_t>(cost) << 32U) | position;
}

// Purpose: Query the minimum cost and earliest position in a bounded half-open interval.
// Inputs: A complete minimum tree, power-of-two leaf base, and admitted local bounds.
// Outputs: Returns the interval minimum, or infinity for an empty interval.
__device__ std::uint64_t range_minimum(const std::uint64_t* tree, std::uint32_t base, std::uint32_t begin,
                                       std::uint32_t end) {
    auto best = kInfinity;
    for (begin += base, end += base; begin < end; begin /= 2U, end /= 2U) {
        if ((begin & 1U) != 0U) {
            best = min(best, tree[begin++]);
        }
        if ((end & 1U) != 0U) {
            best = min(best, tree[--end]);
        }
    }
    return best;
}

// Purpose: Publish a point after all readers of the previous iteration have synchronized.
// Inputs: A complete minimum tree, valid leaf and cost/position key; lane zero is the sole writer.
// Outputs: Updates the leaf and its ancestors with exact interval minima.
__device__ void publish_minimum(std::uint64_t* tree, std::uint32_t base, std::uint32_t position, std::uint64_t value) {
    auto node = base + position;
    tree[node] = value;
    while (node > 1U) {
        node /= 2U;
        tree[node] = min(tree[2U * node], tree[2U * node + 1U]);
    }
}

// Purpose: Retain a portable shared-memory reduction for other hardware wavefront widths.
// Inputs: A lane-local key and exactly 256 shared slots, with all block lanes participating.
// Outputs: Returns the same exact minimum key to every lane after synchronized reduction.
__device__ std::uint64_t shared_block_minimum(std::uint64_t candidate, std::uint64_t* shared) {
    shared[threadIdx.x] = candidate;
    __syncthreads();
    for (auto stride = kLanes / 2U; stride != 0U; stride /= 2U) {
        if (threadIdx.x < stride) {
            shared[threadIdx.x] = min(shared[threadIdx.x], shared[threadIdx.x + stride]);
        }
        __syncthreads();
    }
    return shared[0];
}

// Purpose: Reduce exact cost/position keys within one fully participating hardware wavefront.
// Inputs: A lane-local key and the actual admitted hardware width, either 32 or 64.
// Outputs: Returns the wavefront minimum in lane zero without shared-memory barriers.
__device__ std::uint64_t wavefront_minimum(std::uint64_t candidate, std::uint32_t width) {
    for (auto stride = width / 2U; stride != 0U; stride /= 2U) {
        candidate = min(candidate, __shfl_down(candidate, stride, static_cast<int>(width)));
    }
    return candidate;
}

// Purpose: Reduce Neutron's exact keys with two block barriers on supported hardware wavefront widths.
// Inputs: A lane-local key and 256 shared slots; every block lane participates in each barrier.
// Outputs: Returns the same deterministic minimum to every lane, retaining the shared reduction for other widths.
__device__ std::uint64_t block_minimum(std::uint64_t candidate, std::uint64_t* shared) {
    const auto width = static_cast<std::uint32_t>(warpSize);
    if (width != 32U && width != 64U) {
        return shared_block_minimum(candidate, shared);
    }
    const auto lane = static_cast<std::uint32_t>(threadIdx.x) % width;
    const auto wavefront = static_cast<std::uint32_t>(threadIdx.x) / width;
    const auto partial = wavefront_minimum(candidate, width);
    if (lane == 0U) {
        shared[wavefront] = partial;
    }
    __syncthreads();
    if (wavefront == 0U) {
        const auto staged = lane < kLanes / width ? shared[lane] : kInfinity;
        const auto best = wavefront_minimum(staged, width);
        if (lane == 0U) {
            shared[0] = best;
        }
    }
    __syncthreads();
    return shared[0];
}

// Purpose: Initialize each independent parse tree before descending dynamic programming.
// Inputs: Disjoint admitted per-segment trees; other state is written before each read.
// Outputs: Marks all unavailable candidates with infinity; no uninitialized tree node is consumed.
__global__ void initialize(State state) {
    auto* suffix = state.suffix_tree + blockIdx.x * kTreeWords;
    auto* residues = state.residue_tree + blockIdx.x * kResidueTreeWords;
    for (auto index = static_cast<std::uint32_t>(threadIdx.x); index < kTreeWords; index += kLanes) {
        suffix[index] = kInfinity;
    }
    for (auto index = static_cast<std::uint32_t>(threadIdx.x); index < kResidueTreeWords; index += kLanes) {
        residues[index] = kInfinity;
    }
}

// Purpose: Prove which admitted match graphs need parsing and complete the exact empty-graph decision on HIP.
// Inputs: Verified matches, initialized trees and one reusable status word per independent segment.
// Outputs: Writes a zero/one activity flag; empty graphs receive their sole literal-only parse and exact byte cost.
__global__ void classify_match_graphs(State state, std::uint32_t input_size, std::uint32_t* active_segments) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    const auto start = segment * kSegmentBytes;
    const auto size = min(input_size - start, kSegmentBytes);
    auto candidate = kInfinity;
    for (auto position = static_cast<std::uint32_t>(threadIdx.x); position < size; position += kLanes) {
        const auto match = state.matches[start + position];
        if (size - position >= 12U && match.distance != 0U && match.distance <= position && match.length >= 4U) {
            candidate = key(0U, position);
            break;
        }
    }
    __shared__ std::uint64_t shared[kLanes];
    const auto first_match = block_minimum(candidate, shared);
    if (threadIdx.x == 0U) {
        active_segments[segment] = first_match == kInfinity ? 0U : 1U;
        if (first_match == kInfinity) {
            const auto extension = size < 15U ? 0U : 1U + (size - 15U) / 255U;
            state.next_matches[start] = size;
            publish_minimum(state.suffix_tree + segment * kTreeWords, kSegmentBytes, 0U,
                            key(1U + size + extension, 0U));
        }
    }
}

// Purpose: Choose every allowed match length by exact LZ4 extension-cost plateaus.
// Inputs: Verified matches and already computed later suffixes; lanes cover every plateau in bounded strides.
// Outputs: Publishes M(i) = token/offset/extension cost plus the winning following suffix, and its end position.
__device__ void choose_match(State state, std::uint32_t start, std::uint32_t size, std::uint32_t position,
                             std::uint64_t* shared) {
    const auto absolute = start + position;
    const auto match = state.matches[absolute];
    const auto maximum =
        size - position >= 12U ? min(static_cast<std::uint32_t>(match.length), size - position - 5U) : 0U;
    auto candidate = kInfinity;
    if (match.distance != 0U && match.distance <= position) {
        auto* tree = state.suffix_tree + blockIdx.x * kTreeWords;
        for (auto plateau = static_cast<std::uint32_t>(threadIdx.x);; plateau += kLanes) {
            const auto first = plateau == 0U ? 4U : 19U + (plateau - 1U) * 255U;
            if (first > maximum) {
                break;
            }
            const auto last = plateau == 0U ? 18U : 18U + plateau * 255U;
            const auto suffix =
                range_minimum(tree, kSegmentBytes, position + first, position + min(last, maximum) + 1U);
            if (suffix != kInfinity) {
                candidate = min(candidate, key(static_cast<std::uint32_t>(suffix >> 32U) + 3U + plateau,
                                               static_cast<std::uint32_t>(suffix)));
            }
        }
    }
    const auto best = block_minimum(candidate, shared);
    if (threadIdx.x == 0U) {
        state.match_costs[absolute] = best == kInfinity ? 0xFFFFFFFFU : static_cast<std::uint32_t>(best >> 32U);
        state.match_ends[absolute] = static_cast<std::uint32_t>(best);
    }
    __syncthreads();
}

// Purpose: Admit one newly eligible long literal run using its residue modulo 255.
// Inputs: Descending position and previously computed M(i+15), with one writer per segment.
// Outputs: Retains the minimum M(j)+j+floor(j/255) for each residue among j >= i+15.
__device__ void admit_long_literals(State state, std::uint32_t start, std::uint32_t size, std::uint32_t position) {
    const auto next = position + 15U;
    if (threadIdx.x == 0U && next < size && state.match_costs[start + next] != 0xFFFFFFFFU) {
        auto* tree = state.residue_tree + blockIdx.x * kResidueTreeWords;
        const auto residue = next % 255U;
        const auto value = key(state.match_costs[start + next] + next + next / 255U, next);
        publish_minimum(tree, 256U, residue, min(tree[256U + residue], value));
    }
    __syncthreads();
}

// Purpose: Resolve long literal candidates without enumerating every future match position.
// Inputs: Complete residue minima and the current position; lane 15, 16 or 17 selects one residue interval.
// Outputs: Returns the exact literal-extension-adjusted minimum, or infinity when the interval has no match.
__device__ std::uint64_t long_literal_candidate(const std::uint64_t* tree, std::uint32_t position,
                                                std::uint32_t interval) {
    const auto residue = position % 255U;
    const auto lower = residue > 240U ? residue - 240U : 0U;
    const auto upper = min(residue + 15U, 255U);
    const std::uint32_t boundaries[4]{0U, lower, upper, 255U};
    const auto best = range_minimum(tree, 256U, boundaries[interval], boundaries[interval + 1U]);
    if (best == kInfinity) {
        return best;
    }
    const auto adjusted = static_cast<std::int32_t>(best >> 32U) - static_cast<std::int32_t>(position) -
                          static_cast<std::int32_t>(position / 255U) + static_cast<std::int32_t>(interval) - 1;
    return key(static_cast<std::uint32_t>(adjusted), static_cast<std::uint32_t>(best));
}

// Purpose: Publish the minimum complete sequence cost, including literals, token, offset and following sequences.
// Inputs: Current M(i), admitted long-literal residue minima and completed later M/F values.
// Outputs: Stores the selected next match or final-tail sentinel and publishes F(i) for earlier positions.
__device__ void choose_sequence(State state, std::uint32_t start, std::uint32_t size, std::uint32_t position,
                                std::uint64_t* shared) {
    const auto lane = static_cast<std::uint32_t>(threadIdx.x);
    auto candidate = kInfinity;
    if (lane < 15U && lane < size - position && state.match_costs[start + position + lane] != 0xFFFFFFFFU) {
        candidate = key(state.match_costs[start + position + lane] + lane, position + lane);
    } else if (lane >= 15U && lane <= 17U) {
        candidate = long_literal_candidate(state.residue_tree + blockIdx.x * kResidueTreeWords, position, lane - 15U);
    } else if (lane == kLanes - 1U) {
        const auto literals = size - position;
        const auto extension = literals < 15U ? 0U : (literals - 15U) / 255U + 1U;
        candidate = key(1U + literals + extension, size);
    }
    const auto best = block_minimum(candidate, shared);
    if (threadIdx.x == 0U) {
        state.next_matches[start + position] = static_cast<std::uint32_t>(best);
        publish_minimum(state.suffix_tree + blockIdx.x * kTreeWords, kSegmentBytes, position,
                        key(static_cast<std::uint32_t>(best >> 32U), position));
    }
    __syncthreads();
}

// Purpose: Compute a bounded descending tile with one cooperative block per independent dictionary segment.
// Inputs: Initialized trees, verified matches, activity flags and completed later tiles; remaining_end is local.
// Outputs: Publishes up to 32 positions per segment; no device launch contains the full 64 KiB serial parse.
__global__ void parse_tile(State state, std::uint32_t input_size, std::uint32_t remaining_end,
                           const std::uint32_t* active_segments) {
    if (active_segments[blockIdx.x] == 0U) {
        return;
    }
    const auto start = static_cast<std::uint32_t>(blockIdx.x) * kSegmentBytes;
    const auto size = min(input_size - start, kSegmentBytes);
    const auto begin = remaining_end > kPositionsPerLaunch ? remaining_end - kPositionsPerLaunch : 0U;
    __shared__ std::uint64_t shared[kLanes];
    for (auto end = min(size, remaining_end); end > begin; --end) {
        const auto position = end - 1U;
        choose_match(state, start, size, position, shared);
        admit_long_literals(state, start, size, position);
        choose_sequence(state, start, size, position, shared);
    }
}

// Purpose: Count all required length-extension bytes, including the terminal zero at a nibble boundary.
// Inputs: Bounded literal length or match length minus four.
// Outputs: Returns the exact LZ4 extension size.
__device__ std::uint32_t extension_bytes(std::uint32_t length) {
    return length < 15U ? 0U : (length - 15U) / 255U + 1U;
}

// Purpose: Emit an admitted LZ4 extension without unaligned wide stores.
// Inputs: Output has the exact capacity already validated by the sequence writer.
// Outputs: Writes only extension bytes and returns the next output position.
__device__ std::uint32_t write_extension(std::byte* output, std::uint32_t written, std::uint32_t length) {
    if (length >= 15U) {
        length -= 15U;
        while (length >= 255U) {
            output[written++] = std::byte{255};
            length -= 255U;
        }
        output[written++] = static_cast<std::byte>(length);
    }
    return written;
}

// Purpose: Initialize bounded incremental writers before any output byte is emitted.
// Inputs: One cursor and size word for each admitted segment.
// Outputs: Clears cursors and marks segments as unfinished.
__global__ void initialize_writers(EmitCursor* cursors, std::uint32_t* sizes, std::uint32_t segments) {
    const auto index = static_cast<std::uint32_t>(threadIdx.x);
    if (index < segments) {
        cursors[index] = {};
        sizes[index] = 0U;
    }
}

// Purpose: Materialize at most 64 selected sequences without doing CPU parse or output assembly.
// Inputs: Complete verified decisions, exact input, bounded output slots and persistent writer cursors.
// Outputs: Writes segment bytes incrementally; terminal size must equal F(0), and invariant failures use a sentinel.
__global__ void emit_tile(const std::byte* input, std::uint32_t input_size, State state, std::byte* output,
                          EmitCursor* cursors, std::uint32_t* sizes) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (sizes[segment] != 0U) {
        return;
    }
    const auto start = segment * kSegmentBytes;
    const auto size = min(input_size - start, kSegmentBytes);
    auto* destination = output + segment * kEncodedSegmentCapacity;
    __shared__ EmitCursor cursor;
    __shared__ std::uint32_t next;
    __shared__ std::uint32_t end;
    __shared__ std::uint32_t literal_output;
    __shared__ bool failed;
    if (threadIdx.x == 0U) {
        cursor = cursors[segment];
        failed = false;
    }
    __syncthreads();
    for (std::uint32_t sequence = 0U; sequence < kSequencesPerLaunch; ++sequence) {
        if (threadIdx.x == 0U) {
            failed = cursor.position >= size || cursor.written > kEncodedSegmentCapacity;
            next = failed ? size : state.next_matches[start + cursor.position];
            failed = failed || next < cursor.position || next > size;
            end = failed || next == size ? size : state.match_ends[start + next];
            failed = failed || end < next || end > size ||
                     (next != size && (size - next < 12U || end - next < 4U || size - end < 5U));
            if (!failed && next != size) {
                const auto distance = state.matches[start + next].distance;
                failed = distance == 0U || distance > next;
            }
            if (!failed) {
                const auto literals = next - cursor.position;
                const auto code = next == size ? 0U : end - next - 4U;
                const auto needed =
                    1U + literals + extension_bytes(literals) + (next == size ? 0U : 2U + extension_bytes(code));
                failed = needed > kEncodedSegmentCapacity - cursor.written;
                if (!failed) {
                    destination[cursor.written++] = static_cast<std::byte>((min(literals, 15U) << 4U) | min(code, 15U));
                    literal_output = write_extension(destination, cursor.written, literals);
                    cursor.written = literal_output + literals;
                    if (next != size) {
                        const auto distance = state.matches[start + next].distance;
                        destination[cursor.written++] = static_cast<std::byte>(distance & 255U);
                        destination[cursor.written++] = static_cast<std::byte>(distance >> 8U);
                        cursor.written = write_extension(destination, cursor.written, code);
                    }
                }
            }
        }
        __syncthreads();
        if (failed) {
            if (threadIdx.x == 0U) {
                sizes[segment] = 0xFFFFFFFFU;
            }
            return;
        }
        for (auto byte = static_cast<std::uint32_t>(threadIdx.x); byte < next - cursor.position; byte += kLanes) {
            destination[literal_output + byte] = input[start + cursor.position + byte];
        }
        __syncthreads();
        if (next == size) {
            if (threadIdx.x == 0U) {
                const auto expected = state.suffix_tree[segment * kTreeWords + kSegmentBytes] >> 32U;
                sizes[segment] = cursor.written == expected ? cursor.written : 0xFFFFFFFFU;
            }
            return;
        }
        if (threadIdx.x == 0U) {
            cursor.position = end;
        }
        __syncthreads();
    }
    if (threadIdx.x == 0U) {
        cursors[segment] = cursor;
    }
}

}  // namespace superzip::dictionary::optimal
