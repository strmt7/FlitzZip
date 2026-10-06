#include "gpu/hip_kernel_api.hpp"
#include "gpu/dictionary_matcher.hpp"
#include "gpu/dictionary_device.hpp"
#include "gpu/hip_codec_support.hpp"
#include "gpu/hip_dictionary_optimal.hpp"
#include "gpu/neutron_stage_clock.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <rocprim/rocprim_version.hpp>

namespace superzip::dictionary {
namespace {

using namespace hip_detail;
static_assert(kDictionaryMaxPeriodicMatchBytes <= std::numeric_limits<std::uint16_t>::max());

using DecodeSpan = DictionarySegmentSpan;
static_assert(sizeof(DecodeSpan) == 16U);

// Purpose: Finish a multi-kernel stage without misreporting a library operation as a single kernel launch.
// Inputs: Recorded HIP events surrounding ordered work in the per-thread stream.
// Outputs: Returns valid elapsed milliseconds or nullopt; synchronizes and throws on asynchronous HIP errors.
std::optional<double> finish_dictionary_stage(const HipEventPair& events) {
    check_hip(hipEventSynchronize(events.stop), "synchronize dictionary stage");
    float milliseconds = 0;
    check_hip(hipEventElapsedTime(&milliseconds, events.start, events.stop), "measure dictionary stage");
    return validated_stage_milliseconds(static_cast<double>(milliseconds));
}

// Purpose: Build bounded GPU predecessor links shared by diagnostics and demand-driven encoding.
// Inputs: Validated input, optional already uploaded bytes, additional workspace, and a synchronous consumer.
// Outputs: Returns the consumer's result; owns and releases index allocations, including on failure.
template <typename Consumer>
// Purpose: Execute the documented index consumer. Inputs: Bounded source/workspace. Outputs: Consumed batch.
auto with_dictionary_index(std::span<const std::byte> input, std::size_t consumer_bytes, Consumer consume,
                           const std::byte* borrowed_device_input = nullptr) {
    const auto size = static_cast<std::uint32_t>(input.size());
    const auto blocks = (size + kDictionaryKernelThreads - 1U) / kDictionaryKernelThreads;
    const auto key_bytes = checked_multiply_bytes(input.size(), sizeof(std::uint64_t), "dictionary keys");
    const auto previous_bytes = checked_multiply_bytes(input.size(), sizeof(std::uint32_t), "dictionary links");
    std::size_t temporary_bytes = 0;
    check_hip(hip_kernel_api().sort_dictionary_keys(nullptr, &temporary_bytes, static_cast<std::uint64_t*>(nullptr),
                                                    static_cast<std::uint64_t*>(nullptr), size, hipStreamPerThread),
              "query dictionary radix-sort workspace");
    const auto scratch_bytes = std::max<std::size_t>(temporary_bytes, 1U);
    auto required_bytes = checked_add_bytes(input.size(), key_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, key_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, previous_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, scratch_bytes, "dictionary workspace");
    required_bytes = checked_add_bytes(required_bytes, consumer_bytes, "dictionary consumer workspace");
    if (required_bytes > kMaxWorkspaceBytes) {
        throw GpuError("dictionary workspace exceeds the per-batch GPU memory limit");
    }
    const auto owned_bytes = borrowed_device_input == nullptr ? input.size() : 0U;
    HipDeviceMemoryReservation reservation(required_bytes - input.size() + owned_bytes, "dictionary match finder");
    HipDeviceBuffer<std::byte> device_input;
    auto* source_device = borrowed_device_input;
    if (borrowed_device_input == nullptr) {
        device_input.allocate_checked(input.size(), "allocate dictionary input");
        check_hip(copy_on_codec_stream(device_input.get(), input.data(), input.size(), hipMemcpyHostToDevice),
                  "upload dictionary input");
        source_device = device_input.get();
    }
    HipDeviceBuffer<std::uint64_t> keys(key_bytes, "allocate dictionary keys");
    HipDeviceBuffer<std::uint64_t> sorted_keys(key_bytes, "allocate sorted dictionary keys");
    HipDeviceBuffer<std::uint32_t> previous(previous_bytes, "allocate dictionary links");
    HipDeviceBuffer<std::byte> temporary(scratch_bytes, "allocate dictionary radix workspace");
    auto events = make_hip_event_pair("create dictionary timing events");
    launch_kernel_with_timing(hip_kernel_api().build_dictionary_keys, blocks, kDictionaryKernelThreads, 0,
                              hipStreamPerThread, events.start, nullptr, "launch dictionary keys", source_device, size,
                              keys.get());
    check_hip(hip_kernel_api().sort_dictionary_keys(temporary.get(), &temporary_bytes, keys.get(), sorted_keys.get(),
                                                    size, hipStreamPerThread),
              "sort dictionary keys");
    launch_kernel_with_timing(hip_kernel_api().link_dictionary_predecessors, blocks, kDictionaryKernelThreads, 0,
                              hipStreamPerThread, nullptr, events.stop, "launch dictionary links", sorted_keys.get(),
                              size, previous.get());
    MatchBatch result;
    result.index_ms = finish_dictionary_stage(events);
    result.device_workspace_bytes = required_bytes;
    result.h2d_bytes = owned_bytes;
    result.primitive_version = ROCPRIM_VERSION;
    result.gpu_used = true;
    // The index stage has synchronized; its sorted-key allocation can now hold the consumer's packed output.
    auto consumed = consume(source_device, previous.get(), nullptr, reinterpret_cast<std::byte*>(sorted_keys.get()),
                            key_bytes, result);
    temporary.reset_checked("free dictionary radix workspace");
    previous.reset_checked("free dictionary links");
    sorted_keys.reset_checked("free sorted dictionary keys");
    keys.reset_checked("free dictionary keys");
    device_input.reset_checked("free dictionary input");
    return consumed;
}

// Purpose: Supply sampled distances for demand-verified predecessors without a full link table.
// Inputs: Borrowed device input, one validated distance per segment, consumer workspace, and a synchronous consumer.
// Outputs: Runs the same verified encoder contract with smaller bounded workspace or throws before exposing output.
template <typename Consumer>
// Purpose: Execute the documented index consumer. Inputs: Bounded source/workspace. Outputs: Consumed batch.
auto with_periodic_index(std::span<const std::byte> input, std::span<const std::uint16_t> distances,
                         std::size_t consumer_bytes, Consumer consume, const std::byte* borrowed_device_input) {
    const auto period_bytes = distances.size() * sizeof(std::uint16_t);
    const auto packed_bytes = distances.size() * kEncodedSegmentCapacity;
    const auto periodic_bytes = checked_add_bytes(period_bytes, packed_bytes, "periodic dictionary workspace");
    const auto required_bytes = checked_add_bytes(periodic_bytes, consumer_bytes, "periodic dictionary workspace");
    const auto total_workspace_bytes = checked_add_bytes(input.size(), required_bytes, "periodic dictionary workspace");
    if (total_workspace_bytes > kMaxWorkspaceBytes) {
        throw GpuError("periodic dictionary workspace exceeds the per-batch GPU memory limit");
    }
    HipDeviceMemoryReservation reservation(required_bytes, "periodic dictionary match finder");
    HipDeviceBuffer<std::byte> periodic_buffers(periodic_bytes, "allocate periodic dictionary buffers");
    auto* device_distances = reinterpret_cast<std::uint16_t*>(periodic_buffers.get());
    auto* packed = periodic_buffers.get() + period_bytes;
    check_hip(copy_on_codec_stream(device_distances, distances.data(), period_bytes, hipMemcpyHostToDevice),
              "upload periodic dictionary distances");
    MatchBatch metadata;
    metadata.device_workspace_bytes = total_workspace_bytes;
    metadata.h2d_bytes = period_bytes;
    metadata.gpu_used = true;
    auto consumed = consume(borrowed_device_input, nullptr, device_distances, packed, packed_bytes, metadata);
    periodic_buffers.reset_checked("free periodic dictionary buffers");
    return consumed;
}

// Purpose: Select the periodic or exact-prefix index while sharing the unchanged segment encoder.
// Inputs: Validated source, consumer workspace, optional sampled distances, and borrowed device input.
// Outputs: Returns a fully consumed encoded batch or propagates bounded HIP failures.
template <typename Consumer>
// Purpose: Execute the documented index consumer. Inputs: Bounded source/workspace. Outputs: Consumed batch.
auto with_selected_index(std::span<const std::byte> input, std::size_t consumer_bytes, Consumer consume,
                         const std::byte* borrowed_device_input, std::span<const std::uint16_t> periodic_distances) {
    if (!periodic_distances.empty()) {
        return with_periodic_index(input, periodic_distances, consumer_bytes, consume, borrowed_device_input);
    }
    return with_dictionary_index(input, consumer_bytes, consume, borrowed_device_input);
}

// Purpose: Compact validated device segments and preserve exact resource and event telemetry.
// Inputs: Complete output slots, device sizes, downloaded sizes and an admitted reusable packed allocation.
// Outputs: Returns only used bytes in segment order; rejects capacity mismatches before host output allocation.
PackedEncodedBatch finish_encoded_dictionary(const std::byte* output, const std::uint32_t* sizes,
                                             std::span<const std::uint32_t> host_sizes, std::byte* packed_device,
                                             std::size_t packed_capacity, const MatchBatch& metadata,
                                             std::optional<double> encode_ms, std::uint32_t kernel_launches,
                                             std::size_t downloaded_size_bytes) {
    std::size_t packed_bytes = 0U;
    for (const auto encoded_size : host_sizes) {
        if (encoded_size == 0U || encoded_size > kEncodedSegmentCapacity) {
            throw GpuError("dictionary encoder exceeded its segment capacity");
        }
        packed_bytes = checked_add_bytes(packed_bytes, encoded_size, "dictionary packed payload");
    }
    if (packed_bytes > packed_capacity) {
        throw GpuError("dictionary packed payload exceeds the reserved device buffer");
    }
    auto compact_events = make_hip_event_pair("create dictionary compact timing events");
    launch_measured_kernel(hip_kernel_api().compact_dictionary_segments, static_cast<unsigned int>(host_sizes.size()),
                           kDictionaryKernelThreads, 0, hipStreamPerThread, compact_events,
                           "launch dictionary compaction", output, sizes, static_cast<std::uint32_t>(host_sizes.size()),
                           packed_device);
    const auto compact_ms = finish_dictionary_stage(compact_events);
    std::vector<std::byte> packed(packed_bytes);
    check_hip(copy_on_codec_stream(packed.data(), packed_device, packed_bytes, hipMemcpyDeviceToHost),
              "download packed dictionary blocks");
    PackedEncodedBatch result;
    result.telemetry.device_workspace_bytes = metadata.device_workspace_bytes;
    result.telemetry.h2d_bytes = metadata.h2d_bytes;
    result.telemetry.d2h_bytes = downloaded_size_bytes + packed_bytes;
    result.telemetry.index_ms = metadata.index_ms;
    result.telemetry.encode_ms = encode_ms;
    result.telemetry.compact_ms = compact_ms;
    if (encode_ms && compact_ms && (metadata.primitive_version == 0U || metadata.index_ms)) {
        result.telemetry.device_ms = metadata.index_ms.value_or(0.0) + *encode_ms + *compact_ms;
    }
    result.telemetry.explicit_kernel_launches = kernel_launches;
    result.telemetry.gpu_used = true;
    result.payload = std::move(packed);
    result.segment_sizes.assign(host_sizes.begin(), host_sizes.end());
    return result;
}

}  // namespace

// Purpose: Download diagnostic matches from the shared HIP dictionary search.
// Inputs: A validated immutable batch and bounded effort.
// Outputs: Returns exact match records and resource telemetry; no encoded payload is produced.
MatchBatch find_matches_hip(std::span<const std::byte> input, const Effort& effort) {
    const auto match_bytes = checked_multiply_bytes(input.size(), sizeof(Match), "dictionary matches");
    return with_dictionary_index(
        input, match_bytes,
        [input, effort, match_bytes](const std::byte* device_input, const std::uint32_t* previous, const std::uint16_t*,
                                     std::byte*, std::size_t, MatchBatch metadata) {
            HipDeviceBuffer<Match> matches(match_bytes, "allocate dictionary diagnostic matches");
            const auto size = static_cast<std::uint32_t>(input.size());
            const auto blocks = (size + kDictionaryKernelThreads - 1U) / kDictionaryKernelThreads;
            const auto events = make_hip_event_pair("create dictionary search timing events");
            launch_measured_kernel(hip_kernel_api().search_dictionary_matches, blocks, kDictionaryKernelThreads, 0,
                                   hipStreamPerThread, events, "launch dictionary search", device_input, size, previous,
                                   effort, matches.get());
            metadata.search_ms = finish_dictionary_stage(events);
            metadata.matches.resize(input.size());
            metadata.d2h_bytes = match_bytes;
            check_hip(
                copy_on_codec_stream(metadata.matches.data(), matches.get(), metadata.d2h_bytes, hipMemcpyDeviceToHost),
                "download dictionary matches");
            matches.reset_checked("free dictionary diagnostic matches");
            return metadata;
        });
}

// Purpose: Encode with demand-filled tiled search and download only used bytes plus bounded sizes.
// Inputs: Validated source, effort, optional already uploaded bytes, and optional sampled segment distances.
// Outputs: Returns contiguous encoded segments and resource counters, or throws before exposing incomplete output.
PackedEncodedBatch encode_segments_hip_impl(std::span<const std::byte> input, const Effort& effort,
                                            const std::byte* borrowed_device_input,
                                            std::span<const std::uint16_t> periodic_distances) {
    const auto segment_count = (input.size() + kSegmentBytes - 1U) / kSegmentBytes;
    const auto output_bytes = segment_count * kEncodedSegmentCapacity;
    const auto sizes_bytes = segment_count * sizeof(std::uint32_t);
    const auto consumer_bytes = checked_add_bytes(output_bytes, sizes_bytes, "dictionary encode buffers");
    return with_selected_index(
        input, consumer_bytes,
        [=](const std::byte* device_input, const std::uint32_t* previous, const std::uint16_t* distances,
            std::byte* packed_device, std::size_t packed_capacity, const MatchBatch& metadata) {
            HipDeviceBuffer<std::byte> encode_buffers(consumer_bytes, "allocate dictionary encoded buffers");
            // Size words start at the allocation base; byte slots need no alignment padding.
            auto* sizes = reinterpret_cast<std::uint32_t*>(encode_buffers.get());
            auto* output = encode_buffers.get() + sizes_bytes;
            const auto size = static_cast<std::uint32_t>(input.size());
            auto encode_events = make_hip_event_pair("create dictionary encode timing events");
            // Longer periodic searches run only at selected positions, not at every tile byte.
            if (distances != nullptr) {
                auto periodic_effort = effort;
                periodic_effort.max_byte_comparisons =
                    effort.max_byte_comparisons >= kDictionaryMaxPeriodicMatchBytes / kDictionaryPeriodicComparisonScale
                        ? kDictionaryMaxPeriodicMatchBytes
                        : effort.max_byte_comparisons * kDictionaryPeriodicComparisonScale;
                launch_measured_kernel(hip_kernel_api().encode_dictionary_periodic,
                                       static_cast<unsigned int>(segment_count), kDictionaryKernelThreads, 0,
                                       hipStreamPerThread, encode_events, "launch periodic dictionary encoder",
                                       device_input, size, previous, distances, periodic_effort, output, sizes);
            } else if (effort.max_candidates <= 4U) {
                launch_measured_kernel(hip_kernel_api().encode_dictionary_cached,
                                       static_cast<unsigned int>(segment_count), kDictionaryKernelThreads, 0,
                                       hipStreamPerThread, encode_events, "launch cached dictionary encoder",
                                       device_input, size, previous, distances, effort, output, sizes);
            } else {
                launch_measured_kernel(hip_kernel_api().encode_dictionary_deferred,
                                       static_cast<unsigned int>(segment_count), kDictionaryKernelThreads, 0,
                                       hipStreamPerThread, encode_events, "launch deferred dictionary encoder",
                                       device_input, size, previous, distances, effort, output, sizes);
            }
            const auto encode_ms = finish_dictionary_stage(encode_events);
            std::vector<std::uint32_t> host_sizes(segment_count);
            check_hip(copy_on_codec_stream(host_sizes.data(), sizes, sizes_bytes, hipMemcpyDeviceToHost),
                      "download dictionary encoded sizes");
            auto result = finish_encoded_dictionary(output, sizes, host_sizes, packed_device, packed_capacity, metadata,
                                                    encode_ms, periodic_distances.empty() ? 4U : 2U, sizes_bytes);
            encode_buffers.reset_checked("free dictionary encoded buffers");
            return result;
        },
        borrowed_device_input, periodic_distances);
}

struct NeutronShape {
    std::size_t segments;
    std::size_t tree_bytes;
    std::size_t match_bytes;
    std::size_t decision_bytes;
    std::size_t cursor_bytes;
    std::size_t size_bytes;
    std::size_t output_bytes;
    std::size_t total_bytes;
};

// Purpose: Admit every transient optimal-parse allocation before the index reserves GPU memory.
// Inputs: Nonempty source bounded by the Neutron batch limit.
// Outputs: Returns exact checked byte extents; the shared index also counts source, sorting and packed output.
NeutronShape neutron_shape(std::size_t input_bytes) {
    const auto segments = (input_bytes + kSegmentBytes - 1U) / kSegmentBytes;
    NeutronShape shape{
        .segments = segments,
        .tree_bytes =
            checked_multiply_bytes(segments, (optimal::kTreeWords + optimal::kResidueTreeWords) * sizeof(std::uint64_t),
                                   "Neutron parse trees"),
        .match_bytes = checked_multiply_bytes(input_bytes, sizeof(Match), "Neutron matches"),
        .decision_bytes = checked_multiply_bytes(input_bytes, 3U * sizeof(std::uint32_t), "Neutron decisions"),
        .cursor_bytes = checked_multiply_bytes(segments, sizeof(optimal::EmitCursor), "Neutron cursors"),
        .size_bytes = checked_multiply_bytes(segments, sizeof(std::uint32_t), "Neutron sizes"),
        .output_bytes = checked_multiply_bytes(segments, kEncodedSegmentCapacity, "Neutron output"),
        .total_bytes = 0U};
    for (const auto bytes : {shape.tree_bytes, shape.match_bytes, shape.decision_bytes, shape.cursor_bytes,
                             shape.size_bytes, shape.output_bytes}) {
        shape.total_bytes = checked_add_bytes(shape.total_bytes, bytes, "Neutron consumer workspace");
    }
    return shape;
}

struct NeutronWorkspace {
    HipDeviceBuffer<std::byte> allocation;
    optimal::State state{};
    optimal::EmitCursor* cursors = nullptr;
    std::uint32_t* sizes = nullptr;
    std::byte* output = nullptr;

    // Purpose: Own one aligned allocation for all Neutron arrays with the same synchronized lifetime.
    // Inputs: Exact admitted shape and source byte count.
    // Outputs: Binds disjoint device views or throws before dispatch; the aggregate reservation and bytes are
    // unchanged.
    NeutronWorkspace(const NeutronShape& shape, std::size_t input_bytes)
        : allocation(shape.total_bytes, "allocate Neutron workspace") {
        // Each preceding array's element size preserves the following view's alignment, including odd input sizes.
        static_assert(sizeof(std::uint64_t) % alignof(Match) == 0U);
        static_assert(sizeof(Match) % alignof(std::uint32_t) == 0U);
        static_assert(sizeof(std::uint32_t) % alignof(optimal::EmitCursor) == 0U);
        static_assert(sizeof(optimal::EmitCursor) % alignof(std::uint32_t) == 0U);
        auto* next = allocation.get();
        state.suffix_tree = reinterpret_cast<std::uint64_t*>(next);
        state.residue_tree = state.suffix_tree + shape.segments * optimal::kTreeWords;
        next += shape.tree_bytes;
        state.matches = reinterpret_cast<Match*>(next);
        next += shape.match_bytes;
        state.match_costs = reinterpret_cast<std::uint32_t*>(next);
        state.match_ends = state.match_costs + input_bytes;
        state.next_matches = state.match_ends + input_bytes;
        next += shape.decision_bytes;
        cursors = reinterpret_cast<optimal::EmitCursor*>(next);
        next += shape.cursor_bytes;
        sizes = reinterpret_cast<std::uint32_t*>(next);
        output = next + shape.size_bytes;
    }

    // Purpose: Surface successful-operation release failures before payload publication.
    // Inputs: Completed synchronized work; automatic destruction retains ownership after an error.
    // Outputs: Releases all transient arrays with one checked HIP call before the aggregate reservation is returned.
    void release_checked() {
        allocation.reset_checked("free Neutron workspace");
    }
};

struct NeutronRun {
    NeutronStageClock<HipEventPair> clock{make_hip_event_pair("create Neutron operation events")};
    std::size_t downloaded_size_bytes = 0U;
    std::uint32_t active_segment_mask = 0U;
    std::uint32_t parse_launches = 0U;
    std::uint32_t pruned_segment_mask = 0U;
    std::uint32_t budget_kernel_launches = 0U;
};

template <typename Kernel, typename... Args>
// Purpose: Bound cancellation latency to one measured launch and preserve unavailable event timing.
// Inputs: Admitted kernel/arguments, an operation-owned event pair/accumulator and an optional throwing checkpoint.
// Outputs: Synchronizes and reads timing before reusing events; counts only completed kernels.
void run_neutron_stage(NeutronRun& run, const EncodeCheckpoint& checkpoint, Kernel kernel, unsigned int blocks,
                       Args... args) {
    if (checkpoint) {
        checkpoint();
    }
    // Purpose: Dispatch one stage using the operation's borrowed timing handles.
    // Inputs: Events remain owned by the clock; typed kernel arguments survive its synchronous completion.
    // Outputs: Submits ordered HIP work or throws before the clock permits another dispatch.
    run.clock.measure(
        [&](const HipEventPair& events) {
            launch_measured_kernel(kernel, blocks, kDictionaryKernelThreads, 0, hipStreamPerThread, events,
                                   "launch Neutron stage", args...);
        },
        finish_dictionary_stage);
}

// Purpose: Exclude only complete groups whose verified match intervals cannot beat their existing payload.
// Inputs: Admitted groups, initialized workspace and complete graph activity; cursor storage is not yet live.
// Outputs: Retains every competitive graph, initializes valid losing literals and records actual device work.
void apply_neutron_budgets(std::uint32_t input_bytes, const NeutronShape& shape, NeutronWorkspace& workspace,
                           NeutronRun& run, std::vector<std::uint32_t>& activity,
                           std::span<const NeutronParseBudget> budgets, const EncodeCheckpoint& checkpoint) {
    if (budgets.empty()) {
        return;
    }
    if (std::any_of(activity.begin(), activity.end(), [](auto flag) { return flag > 1U; })) {
        throw GpuError("Neutron match-graph activity is invalid");
    }
    const auto segments = static_cast<unsigned int>(shape.segments);
    run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_bound_match_graphs, segments, workspace.state,
                      input_bytes, workspace.cursors);
    ++run.budget_kernel_launches;
    std::vector<optimal::EmitCursor> bounds(shape.segments);
    check_hip(copy_on_codec_stream(bounds.data(), workspace.cursors, shape.cursor_bytes, hipMemcpyDeviceToHost),
              "download Neutron match-graph lower bounds");
    run.downloaded_size_bytes += shape.cursor_bytes;
    std::size_t first = 0U;
    for (const auto& budget : budgets) {
        const auto count = (budget.input_bytes + kSegmentBytes - 1U) / kSegmentBytes;
        std::uint64_t minimum_bytes = 0U;
        for (std::size_t segment = first; segment < first + count; ++segment) {
            const auto size = std::min<std::size_t>(input_bytes - segment * kSegmentBytes, kSegmentBytes);
            const auto& bound = bounds[segment];
            if (bound.position > size) {
                throw GpuError("Neutron match-graph literal bound exceeds its source");
            }
            const auto extensions = bound.position <= 14U ? 0U : (bound.position - 14U + 254U) / 255U;
            if (bound.written != bound.position + 1U + extensions) {
                throw GpuError("Neutron match-graph payload bound is inconsistent");
            }
            minimum_bytes += bound.written;
        }
        if (minimum_bytes >= budget.maximum_payload_bytes) {
            for (std::size_t segment = first; segment < first + count; ++segment) {
                if (activity[segment] != 0U) {
                    run.pruned_segment_mask |= 1U << segment;
                    activity[segment] = 0U;
                }
            }
        }
        first += count;
    }
    if (run.pruned_segment_mask != 0U) {
        run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_initialize_noncompetitive_graphs, segments,
                          workspace.state, input_bytes, run.pruned_segment_mask, workspace.sizes);
        ++run.budget_kernel_launches;
    }
}

// Purpose: Execute deeper matches, competitive exact parsing and incremental output entirely on HIP.
// Inputs: Borrowed source/index, admitted workspace and optional group limits; host controls admission only.
// Outputs: Returns completed segment lengths and real launch/timing/transfer accounting or throws.
std::vector<std::uint32_t> run_neutron_encoder(const std::byte* source, const std::uint32_t* previous,
                                               std::uint32_t input_bytes, const NeutronShape& shape,
                                               NeutronWorkspace& workspace, NeutronRun& run,
                                               const EncodeCheckpoint& checkpoint,
                                               std::span<const NeutronParseBudget> budgets) {
    const auto segments = static_cast<unsigned int>(shape.segments);
    run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_initialize, segments, workspace.state);
    for (std::uint32_t first = 0U; first < input_bytes; first += optimal::kSearchPositionsPerLaunch) {
        const auto count = std::min(optimal::kSearchPositionsPerLaunch, input_bytes - first);
        run_neutron_stage(run, checkpoint, hip_kernel_api().search_neutron_matches,
                          (count + kDictionaryKernelThreads - 1U) / kDictionaryKernelThreads, source, input_bytes,
                          previous, first, count, workspace.state.matches);
    }
    run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_classify_match_graphs, segments, workspace.state,
                      input_bytes, workspace.sizes);
    std::vector<std::uint32_t> host_sizes(shape.segments);
    check_hip(copy_on_codec_stream(host_sizes.data(), workspace.sizes, shape.size_bytes, hipMemcpyDeviceToHost),
              "download Neutron match-graph activity");
    run.downloaded_size_bytes += shape.size_bytes;
    apply_neutron_budgets(input_bytes, shape, workspace, run, host_sizes, budgets, checkpoint);
    std::uint32_t parse_bytes = 0U;
    for (std::size_t segment = 0U; segment < host_sizes.size(); ++segment) {
        const auto active = host_sizes[segment];
        if (active > 1U) {
            throw GpuError("Neutron match-graph activity is invalid");
        }
        if (active != 0U) {
            run.active_segment_mask |= 1U << segment;
            const auto bytes = std::min<std::size_t>(input_bytes - segment * kSegmentBytes, kSegmentBytes);
            parse_bytes = std::max(parse_bytes, static_cast<std::uint32_t>(bytes));
        }
    }
    const auto segment_bytes = std::min(input_bytes, kSegmentBytes);
    for (auto end = parse_bytes; end != 0U;) {
        run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_parse_tile, segments, workspace.state, input_bytes,
                          end, workspace.sizes);
        ++run.parse_launches;
        end = end > optimal::kPositionsPerLaunch ? end - optimal::kPositionsPerLaunch : 0U;
    }
    run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_initialize_writers, 1U, workspace.cursors,
                      workspace.sizes, segments);
    const auto maximum_launches =
        (segment_bytes / 4U + 1U + optimal::kSequencesPerLaunch - 1U) / optimal::kSequencesPerLaunch;
    for (std::uint32_t tile = 0U; tile < maximum_launches; ++tile) {
        run_neutron_stage(run, checkpoint, hip_kernel_api().neutron_emit_tile, segments, source, input_bytes,
                          workspace.state, workspace.output, workspace.cursors, workspace.sizes);
        check_hip(copy_on_codec_stream(host_sizes.data(), workspace.sizes, shape.size_bytes, hipMemcpyDeviceToHost),
                  "download Neutron writer status");
        run.downloaded_size_bytes += shape.size_bytes;
        if (std::all_of(host_sizes.begin(), host_sizes.end(), [](auto bytes) { return bytes != 0U; })) {
            return host_sizes;
        }
    }
    throw GpuError("Neutron segment writer exceeded its admitted sequence count");
}

// Purpose: Reuse the exact-prefix index and aggregate admission for a stronger minimum-byte segment encoder.
// Inputs: Admitted source, borrowed HIP mirror, cancellation checkpoint and complete optional group limits.
// Outputs: Returns contiguous LZ4 segments; all allocations unwind before the index reservation is released.
PackedEncodedBatch encode_neutron_segments_hip_impl(std::span<const std::byte> input,
                                                    const std::byte* borrowed_device_input,
                                                    const EncodeCheckpoint& checkpoint,
                                                    std::span<const NeutronParseBudget> budgets) {
    const auto shape = neutron_shape(input.size());
    return with_dictionary_index(
        input, shape.total_bytes,
        [&](const std::byte* source, const std::uint32_t* previous, const std::uint16_t*, std::byte* packed,
            std::size_t packed_capacity, const MatchBatch& metadata) {
            NeutronWorkspace workspace(shape, input.size());
            NeutronRun run;
            const auto sizes = run_neutron_encoder(source, previous, static_cast<std::uint32_t>(input.size()), shape,
                                                   workspace, run, checkpoint, budgets);
            if (checkpoint) {
                checkpoint();
            }
            auto result = finish_encoded_dictionary(workspace.output, workspace.sizes, sizes, packed, packed_capacity,
                                                    metadata, run.clock.milliseconds(), run.clock.launches() + 3U,
                                                    run.downloaded_size_bytes);
            result.telemetry.neutron_active_segment_mask = run.active_segment_mask;
            result.telemetry.neutron_parse_launches = run.parse_launches;
            result.telemetry.neutron_pruned_segment_mask = run.pruned_segment_mask;
            result.telemetry.neutron_budget_kernel_launches = run.budget_kernel_launches;
            workspace.release_checked();
            return result;
        },
        borrowed_device_input);
}

// Purpose: Expose contiguous downloaded dictionary payloads as independent diagnostic segments.
// Inputs: Exact source extent and an already validated packed result.
// Outputs: Returns unchanged telemetry and bounded per-segment owned payloads.
EncodedBatch unpack_dictionary_batch(std::span<const std::byte> input, PackedEncodedBatch packed) {
    auto result = std::move(packed.telemetry);
    result.segments.reserve(packed.segment_sizes.size());
    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < packed.segment_sizes.size(); ++index) {
        EncodedSegment segment;
        segment.input_bytes =
            static_cast<std::uint32_t>(std::min(input.size() - index * kSegmentBytes, std::size_t{kSegmentBytes}));
        segment.payload.assign(packed.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                               packed.payload.begin() +
                                   static_cast<std::ptrdiff_t>(offset + packed.segment_sizes[index]));
        offset += packed.segment_sizes[index];
        result.segments.push_back(std::move(segment));
    }
    return result;
}

// Purpose: Preserve the ordinary standalone dictionary encoder's owned-upload contract.
// Inputs: Validated source and effort.
// Outputs: Returns independent LZ4 blocks with one owned host-to-device source upload.
EncodedBatch encode_segments_hip(std::span<const std::byte> input, const Effort& effort) {
    return unpack_dictionary_batch(input, encode_segments_hip_impl(input, effort, nullptr, {}));
}

// Purpose: Expose the bounded minimum-byte encoder through the same independent-segment diagnostic contract.
// Inputs: Admitted source, throwing checkpoint and complete optional group limits.
// Outputs: Returns complete GPU segments with one owned upload and no CPU parse.
EncodedBatch encode_neutron_segments_hip(std::span<const std::byte> input, const EncodeCheckpoint& checkpoint,
                                         std::span<const NeutronParseBudget> budgets) {
    return unpack_dictionary_batch(input, encode_neutron_segments_hip_impl(input, nullptr, checkpoint, budgets));
}

// Purpose: Admit the production borrowed-input boundary before any stronger search or parse allocation.
// Inputs: Bounded host/device mirrors, throwing checkpoint and complete optional group limits.
// Outputs: Returns complete GPU segments or rejects invalid spans; CPU fallback is never used.
PackedEncodedBatch encode_neutron_segments_from_device_hip(std::span<const std::byte> input,
                                                           const std::byte* device_input,
                                                           const EncodeCheckpoint& checkpoint,
                                                           std::span<const NeutronParseBudget> budgets) {
    if (input.empty() || input.size() > kMaxNeutronBatchBytes || device_input == nullptr) {
        throw GpuError("Neutron dictionary device encoding request is invalid");
    }
    validate_neutron_budgets(input.size(), budgets);
    if (checkpoint) {
        checkpoint();
    }
    return encode_neutron_segments_hip_impl(input, device_input, checkpoint, budgets);
}

// Purpose: Encode a production candidate from bytes already uploaded by the native HIP pipeline.
// Inputs: Bounded host/device mirrors, validated effort, and optional admitted segment distances.
// Outputs: Returns contiguous independent LZ4 blocks without another source upload.
PackedEncodedBatch encode_segments_from_device_hip(std::span<const std::byte> input, const std::byte* device_input,
                                                   const Effort& effort,
                                                   std::span<const std::uint16_t> periodic_distances) {
    const auto maximum_bytes = periodic_distances.empty() ? kMaxBatchBytes : kMaxPeriodicBatchBytes;
    if (input.empty() || input.size() > maximum_bytes || device_input == nullptr) {
        throw GpuError("dictionary device encoding request is invalid");
    }
    if (!periodic_distances.empty()) {
        if (periodic_distances.size() != (input.size() + kSegmentBytes - 1U) / kSegmentBytes) {
            throw GpuError("periodic dictionary distances do not cover every segment");
        }
        for (std::size_t segment = 0U; segment < periodic_distances.size(); ++segment) {
            const auto size = std::min<std::size_t>(kSegmentBytes, input.size() - segment * kSegmentBytes);
            if (periodic_distances[segment] < kMinMatchBytes ||
                static_cast<std::size_t>(periodic_distances[segment]) * 2U > size) {
                throw GpuError("periodic dictionary distance is outside its segment");
            }
        }
    }
    return encode_segments_hip_impl(input, effort, device_input, periodic_distances);
}

// Purpose: Decode validated archive segments in existing device buffers with bounded transient workspace.
// Inputs: `encoded` and `decoded` are live HIP buffers; `spans` contains validated absolute disjoint extents.
// Outputs: Records telemetry and writes device output, or throws if any segment fails GPU validation.
void decode_segments_device(const std::byte* encoded, std::span<const DictionarySegmentSpan> spans, std::byte* decoded,
                            GpuTelemetry* telemetry) {
    if (spans.empty()) {
        return;
    }
    const auto span_bytes = checked_multiply_bytes(spans.size(), sizeof(DecodeSpan), "dictionary decode spans");
    const auto status_bytes = checked_multiply_bytes(spans.size(), sizeof(std::uint32_t), "dictionary decode statuses");
    HipDeviceMemoryReservation reservation(checked_add_bytes(span_bytes, status_bytes, "dictionary decode scratch"),
                                           "dictionary decode scratch");
    HipDeviceBuffer<DecodeSpan> device_spans(span_bytes, "allocate dictionary decode spans");
    HipDeviceBuffer<std::uint32_t> statuses(status_bytes, "allocate dictionary decode statuses");
    record_gpu_device_allocation_bytes(telemetry, span_bytes + status_bytes);
    check_hip(copy_on_codec_stream(device_spans.get(), spans.data(), span_bytes, hipMemcpyHostToDevice),
              "upload dictionary decode spans");
    record_gpu_h2d_bytes(telemetry, span_bytes);
    auto events = make_hip_event_pair("create dictionary archive decode events");
    launch_measured_kernel(hip_kernel_api().decode_dictionary_segments, static_cast<unsigned int>(spans.size()),
                           kDictionaryKernelThreads, 0, hipStreamPerThread, events, "launch dictionary archive decoder",
                           encoded, device_spans.get(), decoded, statuses.get());
    finish_measured_kernel(telemetry, events, "synchronize dictionary archive decoder");
    std::vector<std::uint32_t> host_statuses(spans.size());
    check_hip(copy_on_codec_stream(host_statuses.data(), statuses.get(), status_bytes, hipMemcpyDeviceToHost),
              "download dictionary archive decode statuses");
    record_gpu_d2h_bytes(telemetry, status_bytes);
    if (std::any_of(host_statuses.begin(), host_statuses.end(), [](auto status) { return status != 1U; })) {
        throw ArchiveError("dictionary block failed GPU decoding validation");
    }
    statuses.reset_checked("free dictionary decode statuses");
    device_spans.reset_checked("free dictionary decode spans");
}

// Purpose: Upload bounded dictionary input and reuse production dispatch, validation, and timing before publication.
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
    HipDeviceMemoryReservation reservation(encoded.size() + decoded_bytes, "dictionary decoder");
    HipDeviceBuffer<std::byte> input(encoded.size(), "allocate dictionary decode input");
    HipDeviceBuffer<std::byte> output(decoded_bytes, "allocate dictionary decode output");
    GpuTelemetry telemetry;
    record_gpu_device_allocation_bytes(&telemetry, encoded.size() + decoded_bytes);
    check_hip(copy_on_codec_stream(input.get(), encoded.data(), encoded.size(), hipMemcpyHostToDevice),
              "upload dictionary blocks");
    record_gpu_h2d_bytes(&telemetry, encoded.size());
    decode_segments_device(input.get(), spans, output.get(), &telemetry);
    DecodedBatch result;
    result.bytes.resize(decoded_bytes);
    check_hip(copy_on_codec_stream(result.bytes.data(), output.get(), decoded_bytes, hipMemcpyDeviceToHost),
              "download dictionary decoded bytes");
    record_gpu_d2h_bytes(&telemetry, decoded_bytes);
    const auto counters = snapshot_gpu_telemetry(telemetry);
    result.decode_ms = validated_stage_milliseconds(counters.kernel_ms);
    result.device_workspace_bytes = required_bytes;
    result.h2d_bytes = counters.h2d_bytes;
    result.d2h_bytes = counters.d2h_bytes;
    result.gpu_used = true;
    output.reset_checked("free dictionary decode output");
    input.reset_checked("free dictionary decode input");
    return result;
}

}  // namespace superzip::dictionary
