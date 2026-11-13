#include "gpu/hip_rans_encoder.hpp"

#include "core/rans_block.hpp"

#include <algorithm>

namespace flitzzip::hip_detail {
namespace {

constexpr std::size_t kEncoderModelBytes =
    256U * sizeof(RansModelRow) + 65536U + (65536U + 66048U + 65536U + 256U) * sizeof(std::uint32_t);

void encoder_checkpoint(const std::function<void()>& checkpoint) {
    if (checkpoint) {
        checkpoint();
    }
}

std::span<const std::byte> admit_encoder_source(std::span<const std::byte> source) {
    if (source.data() == nullptr || source.empty() || source.size() > kRansCountMaxBytes) {
        throw ArchiveError("rANS encoder source extent is invalid");
    }
    require_hip_kernel_device_support();
    return source;
}

std::uint32_t encoder_segments(std::size_t bytes, std::uint32_t shift) {
    return (static_cast<std::uint32_t>(bytes - 1U) >> shift) + 1U;
}

std::size_t aligned_slot_bytes(std::size_t bytes) {
    return (bytes + 3U) & ~std::size_t{3U};
}

void admit_model_frequencies(std::span<const std::uint32_t> frequencies, const RansModelRow& row,
                             std::uint32_t precision) {
    std::uint32_t alphabet = 0U;
    std::uint64_t mass = 0U;
    for (const auto frequency : frequencies) {
        if (frequency == 0U) {
            continue;
        }
        ++alphabet;
        mass += frequency;
        if (frequency > (1U << precision) || (row.alphabet_size != 1U && frequency >= 65536U)) {
            throw ArchiveError("rANS serialized frequency exceeds its precision or wire width");
        }
    }
    const auto expected_mass = row.alphabet_size == 0U ? 0U : 1U << precision;
    if (alphabet != row.alphabet_size || mass != expected_mass) {
        throw ArchiveError("rANS serialized model mass or alphabet is invalid");
    }
}

// Frequencies have already passed complete-row admission. Each row now writes
// exactly its charged sparse extent, without speculative vector growth.
void append_model_row(std::vector<std::byte>& payload, std::span<const std::uint32_t> frequencies,
                      std::uint32_t context, const RansModelRow& row) {
    payload.push_back(static_cast<std::byte>(context));
    payload.push_back(static_cast<std::byte>(row.alphabet_size - 1U));
    for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
        const auto frequency = frequencies[symbol];
        if (frequency == 0U) {
            continue;
        }
        payload.push_back(static_cast<std::byte>(symbol));
        if (row.alphabet_size != 1U) {
            payload.push_back(static_cast<std::byte>(frequency & 255U));
            payload.push_back(static_cast<std::byte>(frequency >> 8U));
        }
    }
}

}  // namespace

RansEncodeWorkspaceRequirements rans_encode_workspace_requirements(std::size_t source_bytes) {
    if (source_bytes == 0U || source_bytes > kRansCountMaxBytes) {
        throw ArchiveError("rANS encoder workspace source extent is invalid");
    }
    const auto segments = encoder_segments(source_bytes, 8U);
    const auto slots = 2U * source_bytes + 4U * segments;
    const auto metadata = segments * sizeof(RansEncodeResult) + (segments + 1U) * sizeof(std::uint32_t);
    const auto scratch = kEncoderModelBytes + aligned_slot_bytes(slots) + metadata + source_bytes;
    const auto host =
        sizeof(RansEncoder) + 65536U * sizeof(std::uint32_t) + (segments + 1U) * sizeof(std::uint32_t) + source_bytes;
    return {host, scratch + rans_statistics_workspace_requirements().device_bytes, scratch, segments,
            static_cast<std::uint32_t>(slots)};
}

RansEncoder::RansEncoder(std::span<const std::byte> device_source, GpuTelemetry* telemetry,
                         const std::function<void()>& checkpoint)
    : source_(admit_encoder_source(device_source)), telemetry_(telemetry),
      required_(rans_encode_workspace_requirements(source_.size())),
      reservation_(required_.scratch_bytes, "rANS encoder scratch"), statistics_(source_, telemetry, checkpoint),
      storage_(required_.scratch_bytes, "allocate rANS encoder scratch"), frequencies_(65536U),
      offsets_(required_.max_segments + 1U) {
    check_hip(hipGetDevice(&device_), "get rANS encoder device");
    record_gpu_device_allocation_bytes(telemetry_, required_.scratch_bytes);
    bind_storage();
}

void RansEncoder::bind_storage() {
    static_assert(alignof(RansModelRow) <= alignof(std::uint32_t));
    static_assert(alignof(RansEncodeResult) <= alignof(std::uint32_t));
    auto* next = storage_.get();
    device_rows_ = reinterpret_cast<RansModelRow*>(next);
    next += 256U * sizeof(RansModelRow);
    order_ = reinterpret_cast<std::uint8_t*>(next);
    next += 65536U;
    device_frequencies_ = reinterpret_cast<std::uint32_t*>(next);
    next += 65536U * sizeof(std::uint32_t);
    models_ = reinterpret_cast<std::uint32_t*>(next);
    next += 66048U * sizeof(std::uint32_t);
    reciprocals_ = reinterpret_cast<std::uint32_t*>(next);
    next += 65536U * sizeof(std::uint32_t);
    device_statuses_ = reinterpret_cast<RansModelStatus*>(next);
    next += 256U * sizeof(RansModelStatus);
    slots_ = next;
    next += aligned_slot_bytes(required_.slot_bytes);
    device_results_ = reinterpret_cast<RansEncodeResult*>(next);
    next += required_.max_segments * sizeof(RansEncodeResult);
    device_offsets_ = reinterpret_cast<std::uint32_t*>(next);
    output_ = next + (required_.max_segments + 1U) * sizeof(std::uint32_t);
}

void RansEncoder::require_current_device() const {
    int current = -1;
    check_hip(hipGetDevice(&current), "get current rANS encoder device");
    if (!reusable_ || current != device_) {
        throw GpuError("rANS encoder is closed or has a different current device");
    }
}

void RansEncoder::initialize_statuses(std::uint32_t count) {
    auto active = std::span(statuses_).first(count);
    std::ranges::fill(active, RansModelStatus::invalid_plan);
    check_hip(copy_on_codec_stream(device_statuses_, active.data(), active.size_bytes(), hipMemcpyHostToDevice),
              "initialize rANS encoder statuses");
    record_gpu_h2d_bytes(telemetry_, active.size_bytes());
}

void RansEncoder::require_statuses(std::uint32_t count) {
    auto active = std::span(statuses_).first(count);
    check_hip(copy_on_codec_stream(active.data(), device_statuses_, active.size_bytes(), hipMemcpyDeviceToHost),
              "read rANS encoder statuses");
    record_gpu_d2h_bytes(telemetry_, active.size_bytes());
    if (std::ranges::any_of(active, [](auto status) { return status != RansModelStatus::success; })) {
        throw ArchiveError("rANS encoder failed GPU model or stream validation");
    }
}

void RansEncoder::prepare_rows(std::uint32_t bits, std::uint32_t shift, const std::function<void()>& checkpoint) {
    // Unconditional counts are independent of segment resets; canonicalize
    // their statistics identity so all shifts reuse the same prepared model.
    const auto model_shift = bits == 0U ? 8U : shift;
    if (bits_ == bits && shift_ == model_shift) {
        return;
    }
    bits_ = 9U;
    precision_ = 0U;
    counts_ = statistics_.project(bits, model_shift, checkpoint);
    const auto contexts = 1U << bits;
    auto rows = std::span(rows_).first(contexts);
    std::ranges::fill(rows, RansModelRow{0U, 0U, RansModelStatus::invalid_plan});
    check_hip(copy_on_codec_stream(device_rows_, rows.data(), rows.size_bytes(), hipMemcpyHostToDevice),
              "initialize rANS model rows");
    record_gpu_h2d_bytes(telemetry_, rows.size_bytes());
    const auto words = contexts * 256U;
    dispatch_codec_kernel(telemetry_, hip_kernel_api().prepare_rans_models, contexts, 256U, "prepare rANS models",
                          counts_.data(), words, static_cast<std::uint32_t>(source_.size()), order_, words,
                          device_rows_, contexts);
    check_hip(copy_on_codec_stream(rows.data(), device_rows_, rows.size_bytes(), hipMemcpyDeviceToHost),
              "read rANS model rows");
    record_gpu_d2h_bytes(telemetry_, rows.size_bytes());
    if (!rans_model_rows_admitted(rows, static_cast<std::uint32_t>(source_.size()), bits)) {
        throw ArchiveError("rANS model rows do not cover the source");
    }
    model_bytes_ = 0U;
    for (const auto& row : rows) {
        if (row.alphabet_size != 0U) {
            model_bytes_ += row.alphabet_size == 1U ? 3U : 2U + 3U * row.alphabet_size;
        }
    }
    encoder_checkpoint(checkpoint);
    bits_ = bits;
    shift_ = model_shift;
}

void RansEncoder::prepare_model(std::uint32_t precision, const std::function<void()>& checkpoint) {
    if (precision_ == precision) {
        return;
    }
    precision_ = 0U;
    const auto contexts = 1U << bits_;
    const auto words = contexts * 256U;
    initialize_statuses(contexts);
    dispatch_codec_kernel(
        telemetry_, hip_kernel_api().normalize_rans_models, contexts, 256U, "normalize rANS encoder models",
        counts_.data(), words, static_cast<const std::uint8_t*>(order_), words,
        static_cast<const RansModelRow*>(device_rows_), contexts, static_cast<std::uint32_t>(source_.size()), precision,
        device_frequencies_, words, device_statuses_, contexts);
    require_statuses(contexts);
    encoder_checkpoint(checkpoint);
    initialize_statuses(contexts);
    dispatch_codec_kernel(telemetry_, hip_kernel_api().build_rans_models, contexts, 256U, "build rANS encoder models",
                          static_cast<const std::uint32_t*>(device_frequencies_), words,
                          static_cast<const RansModelRow*>(device_rows_), contexts, precision, models_, contexts * 258U,
                          reciprocals_, words, device_statuses_, contexts);
    require_statuses(contexts);
    check_hip(copy_on_codec_stream(frequencies_.data(), device_frequencies_, words * sizeof(std::uint32_t),
                                   hipMemcpyDeviceToHost),
              "read rANS encoder frequencies");
    record_gpu_d2h_bytes(telemetry_, words * sizeof(std::uint32_t));
    for (std::uint32_t context = 0U; context < contexts; ++context) {
        admit_model_frequencies(std::span(frequencies_).subspan(context * 256U, 256U), rows_[context], precision);
    }
    encoder_checkpoint(checkpoint);
    precision_ = precision;
}

void RansEncoder::admit_stream_batch(std::uint32_t first, std::uint32_t count, std::uint32_t shift) {
    auto active = std::span(results_).first(count);
    check_hip(copy_on_codec_stream(active.data(), device_results_ + first, active.size_bytes(), hipMemcpyDeviceToHost),
              "read rANS encoded extents");
    record_gpu_d2h_bytes(telemetry_, active.size_bytes());
    for (std::uint32_t i = 0U; i < count; ++i) {
        const auto segment = first + i;
        const auto begin = segment << shift;
        const auto length = std::min(1U << shift, static_cast<std::uint32_t>(source_.size()) - begin);
        const auto slot_begin = 2U * begin + 4U * segment;
        const auto slot_end = slot_begin + 2U * length + 4U;
        const auto& result = active[i];
        if (result.status != RansModelStatus::success || result.stream_offset < slot_begin ||
            result.stream_offset > slot_end || result.stream_bytes != slot_end - result.stream_offset ||
            result.stream_bytes < 4U || result.stream_bytes > required_.slot_bytes - offsets_[segment]) {
            throw ArchiveError("rANS encoded stream extent is invalid");
        }
        offsets_[segment + 1U] = offsets_[segment] + result.stream_bytes;
    }
}

bool RansEncoder::encode_streams(std::uint32_t shift, std::size_t frame_header_bytes, std::size_t exclusive_limit,
                                 const std::function<void()>& checkpoint) {
    const auto segments = encoder_segments(source_.size(), shift);
    const auto slots = static_cast<std::uint32_t>(2U * source_.size() + 4U * segments);
    const auto contexts = 1U << bits_;
    offsets_[0] = 0U;
    for (std::uint32_t first = 0U; first < segments;) {
        const auto count = std::min(kRansEncodeBatchSegments, segments - first);
        auto active = std::span(results_).first(count);
        std::ranges::fill(active, RansEncodeResult{0U, 0U, RansModelStatus::invalid_plan});
        check_hip(
            copy_on_codec_stream(device_results_ + first, active.data(), active.size_bytes(), hipMemcpyHostToDevice),
            "initialize rANS encoded extents");
        record_gpu_h2d_bytes(telemetry_, active.size_bytes());
        dispatch_codec_kernel(telemetry_, hip_kernel_api().encode_rans_segments, (count + 63U) / 64U, 64U,
                              "encode rANS streams", source_.data(), static_cast<std::uint32_t>(source_.size()),
                              static_cast<const std::uint32_t*>(models_), contexts * 258U,
                              static_cast<const std::uint32_t*>(reciprocals_), contexts * 256U, precision_, bits_,
                              shift, first, count, slots_, slots, device_results_, segments);
        admit_stream_batch(first, count, shift);
        first += count;
        encoder_checkpoint(checkpoint);
        if (frame_header_bytes + offsets_[first] + 4U * (segments - first) >= exclusive_limit) {
            return false;
        }
    }
    return true;
}

void RansEncoder::compact_streams(std::uint32_t shift, const std::function<void()>& checkpoint) {
    const auto segments = encoder_segments(source_.size(), shift);
    const auto offsets = std::span(offsets_).first(segments + 1U);
    check_hip(copy_on_codec_stream(device_offsets_, offsets.data(), offsets.size_bytes(), hipMemcpyHostToDevice),
              "upload rANS dense offsets");
    record_gpu_h2d_bytes(telemetry_, offsets.size_bytes());
    const auto slots = static_cast<std::uint32_t>(2U * source_.size() + 4U * segments);
    for (std::uint32_t first = 0U; first < segments;) {
        const auto count = std::min(kRansEncodeBatchSegments, segments - first);
        initialize_statuses(count);
        dispatch_codec_kernel(telemetry_, hip_kernel_api().compact_rans_segments, count + 1U, 256U,
                              "compact rANS streams", static_cast<const std::byte*>(slots_), slots,
                              static_cast<std::uint32_t>(source_.size()), shift,
                              static_cast<const RansEncodeResult*>(device_results_), segments,
                              static_cast<const std::uint32_t*>(device_offsets_), segments + 1U, first, count, output_,
                              offsets.back(), device_statuses_, count);
        require_statuses(count);
        encoder_checkpoint(checkpoint);
        first += count;
    }
}

std::vector<std::byte> RansEncoder::serialize(std::uint32_t shift, std::size_t frame_header_bytes) {
    const auto segments = encoder_segments(source_.size(), shift);
    const auto contexts = 1U << bits_;
    const auto rows = std::span(rows_).first(contexts);
    const auto present = std::ranges::count_if(rows, [](const auto& row) { return row.alphabet_size != 0U; });
    std::vector<std::byte> payload;
    payload.reserve(frame_header_bytes + offsets_[segments]);
    payload.insert(payload.end(), {static_cast<std::byte>(precision_), static_cast<std::byte>(bits_),
                                   static_cast<std::byte>(shift), static_cast<std::byte>(present - 1)});
    for (std::uint32_t context = 0U; context < contexts; ++context) {
        if (rows[context].alphabet_size != 0U) {
            append_model_row(payload, std::span(frequencies_).subspan(context * 256U, 256U), context, rows[context]);
        }
    }
    for (const auto offset : std::span(offsets_).first(segments + 1U)) {
        append_prefix_u32(payload, offset);
    }
    if (payload.size() != frame_header_bytes) {
        throw ArchiveError("rANS sparse frame extent is inconsistent");
    }
    payload.resize(frame_header_bytes + offsets_[segments]);
    check_hip(
        copy_on_codec_stream(payload.data() + frame_header_bytes, output_, offsets_[segments], hipMemcpyDeviceToHost),
        "read complete rANS payload");
    record_gpu_d2h_bytes(telemetry_, offsets_[segments]);
    (void)GpuRansBlockView(payload, static_cast<std::uint32_t>(source_.size()));
    return payload;
}

std::optional<std::vector<std::byte>> RansEncoder::encode(std::uint32_t precision, std::uint32_t context_bits,
                                                          std::uint32_t segment_shift, std::size_t exclusive_limit,
                                                          const std::function<void()>& checkpoint) {
    if (precision < 8U || precision > 16U || context_bits > 8U || segment_shift < 8U || segment_shift > 12U ||
        exclusive_limit > source_.size()) {
        throw ArchiveError("rANS encoder options or complete payload budget are invalid");
    }
    require_current_device();
    try {
        encoder_checkpoint(checkpoint);
        const auto segments = encoder_segments(source_.size(), segment_shift);
        if (11U + 8U * segments >= exclusive_limit) {
            encoder_checkpoint(checkpoint);
            return std::nullopt;
        }
        prepare_rows(context_bits, segment_shift, checkpoint);
        const auto frame_header_bytes = 4U + model_bytes_ + 4U * (segments + 1U);
        if (frame_header_bytes + 4U * segments >= exclusive_limit) {
            encoder_checkpoint(checkpoint);
            return std::nullopt;
        }
        prepare_model(precision, checkpoint);
        if (!encode_streams(segment_shift, frame_header_bytes, exclusive_limit, checkpoint)) {
            return std::nullopt;
        }
        compact_streams(segment_shift, checkpoint);
        auto payload = serialize(segment_shift, frame_header_bytes);
        encoder_checkpoint(checkpoint);
        return payload;
    } catch (...) {
        bits_ = 9U;
        precision_ = 0U;
        counts_ = {};
        reusable_ = hipStreamSynchronize(hipStreamPerThread) == hipSuccess;
        throw;
    }
}

void RansEncoder::close_checked() {
    reusable_ = false;
    bits_ = 9U;
    precision_ = 0U;
    counts_ = {};
    int current = -1;
    check_hip(hipGetDevice(&current), "get rANS encoder close device");
    if (current != device_) {
        throw GpuError("rANS encoder close requires its owning device");
    }
    storage_.reset_checked("free rANS encoder scratch");
    reservation_.reduce_to(0U);
    statistics_.close_checked();
}

}  // namespace flitzzip::hip_detail
