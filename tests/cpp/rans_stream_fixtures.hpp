#pragma once

#include "rans_gpu_fixtures.hpp"
#include "gpu/hip_rans_statistics.hpp"
#include "core/rans_block.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace flitzzip_test {

// Actual device stream storage and guarded reusable metadata. The source is
// uploaded once and never used as scratch. All sizing follows the complete
// block's independent segment/slot equations; buffers are charged through free.
class RansStreamFixture {
  public:
    RansStreamFixture(std::span<const std::byte> source, std::uint32_t segment_shift)
        : bytes(static_cast<std::uint32_t>(source.size())), shift(segment_shift),
          segments(((bytes - 1U) >> shift) + 1U), slot_bytes(2U * bytes + 4U * segments),
          reservation(2U * (std::size_t{bytes} + 2U) + 2U * (std::size_t{slot_bytes} + 2U) +
                          (3U * std::size_t{segments} + 2U + segments + 3U + 514U + 2U * 130U) * sizeof(std::uint32_t),
                      "rANS stream fixture"),
          input(bytes), slots(slot_bytes), result_words(segments * 3U), offsets(segments + 1U), output(slot_bytes),
          decoded(bytes), plans(512U), statuses(128U), decode_statuses(128U) {
        input.reset(source);
    }

    flitzzip::hip_detail::RansEncodeResult* result_data() const {
        return reinterpret_cast<flitzzip::hip_detail::RansEncodeResult*>(result_words.data());
    }
    void encode(RansModelFixture& model, std::uint32_t precision, std::uint32_t bits) {
        using namespace flitzzip::hip_detail;
        slots.reset({});
        result_words.reset({});
        for (std::uint32_t first = 0U; first < segments;) {
            const auto count = std::min(kRansEncodeBatchSegments, segments - first);
            dispatch_codec_kernel(&telemetry, flitzzip::hip_kernel_api().encode_rans_segments, (count + 63U) / 64U, 64U,
                                  "encode rANS stream fixture", input.data(), bytes,
                                  static_cast<const std::uint32_t*>(model.models.data()), model.contexts * 258U,
                                  static_cast<const std::uint32_t*>(model.reciprocals.data()), model.words, precision,
                                  bits, shift, first, count, slots.data(), slot_bytes, result_data(), segments);
            first += count;
        }
    }
    std::vector<std::uint32_t> admit_stream_offsets() {
        const auto results = result_words.read(segments * 3U);
        std::vector<std::uint32_t> dense(segments + 1U);
        for (std::uint32_t i = 0U; i < segments; ++i) {
            REQUIRE_EQ(results[i * 3U + 2U],
                       static_cast<std::uint32_t>(flitzzip::hip_detail::RansModelStatus::success));
            const auto begin = i << shift;
            const auto length = std::min(1U << shift, bytes - begin);
            const auto slot_begin = 2U * begin + 4U * i;
            const auto slot_end = slot_begin + 2U * length + 4U;
            REQUIRE_TRUE(results[i * 3U] >= slot_begin && results[i * 3U] <= slot_end);
            REQUIRE_EQ(results[i * 3U + 1U], slot_end - results[i * 3U]);
            REQUIRE_TRUE(results[i * 3U + 1U] >= 4U);
            REQUIRE_TRUE(results[i * 3U + 1U] <= slot_bytes - dense[i]);
            dense[i + 1U] = dense[i] + results[i * 3U + 1U];
        }
        offsets.reset(dense);
        return dense;
    }
    void compact(std::uint32_t dense_bytes) {
        using namespace flitzzip::hip_detail;
        output.reset({});
        for (std::uint32_t first = 0U; first < segments;) {
            const auto count = std::min(kRansEncodeBatchSegments, segments - first);
            statuses.reset(std::vector<RansModelStatus>(count, RansModelStatus::invalid_plan));
            dispatch_codec_kernel(&telemetry, flitzzip::hip_kernel_api().compact_rans_segments, count + 1U, 256U,
                                  "compact rANS stream fixture", static_cast<const std::byte*>(slots.data()),
                                  slot_bytes, bytes, shift, static_cast<const RansEncodeResult*>(result_data()),
                                  segments, static_cast<const std::uint32_t*>(offsets.data()), segments + 1U, first,
                                  count, output.data(), dense_bytes, statuses.data(), count);
            REQUIRE_EQ(statuses.read(count), std::vector<RansModelStatus>(count, RansModelStatus::success));
            first += count;
        }
    }
    void require_device_decode(RansModelFixture& model, std::span<const std::uint32_t> dense, std::uint32_t precision,
                               std::uint32_t bits, std::span<const std::byte> expected) {
        using namespace flitzzip::hip_detail;
        decoded.reset({});
        for (std::uint32_t first = 0U; first < segments;) {
            const auto count = std::min(kRansEncodeBatchSegments, segments - first);
            std::vector<std::uint32_t> batch(count * 4U);
            for (std::uint32_t i = 0U; i < count; ++i) {
                const auto segment = first + i;
                const auto begin = segment << shift;
                const std::array entry{dense[segment], dense[segment + 1U] - dense[segment], begin,
                                       std::min(1U << shift, bytes - begin)};
                std::ranges::copy(entry, batch.begin() + i * 4U);
            }
            plans.reset(batch);
            decode_statuses.reset(std::vector<RansDecodeStatus>(count, RansDecodeStatus::invalid_plan));
            dispatch_codec_kernel(&telemetry, flitzzip::hip_kernel_api().decode_rans_segments, (count + 63U) / 64U, 64U,
                                  "decode device-produced rANS stream", static_cast<const std::byte*>(output.data()),
                                  dense.back(), static_cast<const std::uint32_t*>(model.models.data()),
                                  model.contexts * 258U, reinterpret_cast<const RansDecodeSegment*>(plans.data()),
                                  count, precision, bits, decoded.data(), bytes, decode_statuses.data());
            REQUIRE_EQ(decode_statuses.read(count), std::vector<RansDecodeStatus>(count, RansDecodeStatus::success));
            first += count;
        }
        REQUIRE_EQ(decoded.read(bytes), std::vector<std::byte>(expected.begin(), expected.end()));
        REQUIRE_EQ(input.read(bytes), std::vector<std::byte>(expected.begin(), expected.end()));
    }
    void release_checked() {
        decode_statuses.release_checked();
        statuses.release_checked();
        plans.release_checked();
        decoded.release_checked();
        output.release_checked();
        offsets.release_checked();
        result_words.release_checked();
        slots.release_checked();
        input.release_checked();
        reservation.reduce_to(0U);
    }

    const std::uint32_t bytes, shift, segments, slot_bytes;
    flitzzip::hip_detail::HipDeviceMemoryReservation reservation;
    CountDeviceBuffer<std::byte> input, slots;
    CountDeviceBuffer<std::uint32_t> result_words, offsets;
    CountDeviceBuffer<std::byte> output, decoded;
    CountDeviceBuffer<std::uint32_t> plans;
    CountDeviceBuffer<flitzzip::hip_detail::RansModelStatus> statuses;
    CountDeviceBuffer<flitzzip::hip_detail::RansDecodeStatus> decode_statuses;
    flitzzip::GpuTelemetry telemetry{flitzzip::GpuObservationMode::CountersOnly};
};

inline void prepare_stream_models(flitzzip::hip_detail::RansStatistics& statistics, RansModelFixture& model,
                                  std::uint32_t bits, std::uint32_t shift, std::uint32_t precision) {
    using namespace flitzzip::hip_detail;
    model.copy_counts(statistics.project(bits, shift));
    model.prepare();
    REQUIRE_TRUE(rans_model_rows_admitted(model.read_rows(), model.bytes, bits));
    model.normalize(precision);
    model.require_status(RansModelStatus::success);
    model.build(precision);
    model.require_status(RansModelStatus::success);
}

// CPU work writes only the approved sparse metadata and offsets around actual
// compacted GPU bytes. It neither normalizes frequencies nor encodes a symbol.
// Independent portable admission/decode then checks the complete wire result.
inline std::vector<std::byte> rans_device_payload(RansModelFixture& model, RansStreamFixture& streams,
                                                  std::span<const std::uint32_t> offsets, std::uint32_t precision,
                                                  std::uint32_t bits) {
    const auto rows = model.read_rows();
    const auto frequencies = model.frequencies.read(model.words);
    const auto present = std::ranges::count_if(rows, [](const auto& row) { return row.alphabet_size != 0U; });
    REQUIRE_TRUE(present >= 1 && present <= 256);
    std::vector<std::byte> payload{static_cast<std::byte>(precision), static_cast<std::byte>(bits),
                                   static_cast<std::byte>(streams.shift), static_cast<std::byte>(present - 1)};
    for (std::uint32_t row = 0U; row < model.contexts; ++row) {
        if (rows[row].alphabet_size == 0U) {
            continue;
        }
        payload.push_back(static_cast<std::byte>(row));
        payload.push_back(static_cast<std::byte>(rows[row].alphabet_size - 1U));
        std::uint32_t alphabet = 0U;
        for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
            const auto frequency = frequencies[row * 256U + symbol];
            if (frequency == 0U) {
                continue;
            }
            ++alphabet;
            payload.push_back(static_cast<std::byte>(symbol));
            if (rows[row].alphabet_size == 1U) {
                REQUIRE_EQ(frequency, 1U << precision);
            } else {
                REQUIRE_TRUE(frequency < 65536U);
                payload.push_back(static_cast<std::byte>(frequency & 255U));
                payload.push_back(static_cast<std::byte>(frequency >> 8U));
            }
        }
        REQUIRE_EQ(alphabet, rows[row].alphabet_size);
    }
    for (const auto offset : offsets) {
        for (unsigned int i = 0U; i < 4U; ++i) {
            payload.push_back(static_cast<std::byte>((offset >> (8U * i)) & 255U));
        }
    }
    const auto encoded = streams.output.read(offsets.back());
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    return payload;
}

inline void require_portable_rans_readback(std::span<const std::byte> payload, std::span<const std::byte> source) {
    const flitzzip::GpuRansBlockView view(payload, static_cast<std::uint32_t>(source.size()));
    std::vector<std::uint32_t> models(view.model_workspace_words());
    std::vector<std::byte> actual(source.size());
    view.decode(actual, models);
    REQUIRE_EQ(actual, std::vector<std::byte>(source.begin(), source.end()));
}

}  // namespace flitzzip_test
