#include "test_util.hpp"
#include "gpu/hip_device.hpp"

#include <array>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "rans_stream_fixtures.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
using namespace flitzzip_test;

std::vector<std::byte> rans_stream_source(std::uint32_t bytes) {
    std::vector<std::byte> source(bytes);
    for (std::uint32_t i = 0U; i < bytes; ++i) {
        source[i] = static_cast<std::byte>((i * 73U + i / 31U) & 15U);
    }
    source.front() = std::byte{0xFF};
    return source;
}

void require_stream_roundtrip(RansStreamFixture& stream, RansModelFixture& model, std::span<const std::byte> source,
                              std::uint32_t precision, std::uint32_t bits) {
    stream.encode(model, precision, bits);
    const auto offsets = stream.admit_stream_offsets();
    stream.compact(offsets.back());
    stream.require_device_decode(model, offsets, precision, bits, source);
    const auto payload = rans_device_payload(model, stream, offsets, precision, bits);
    require_portable_rans_readback(payload, source);
    // Every unused prefix in each private output slot remains its initialized
    // guard. This challenges backwards emission as well as outer allocation bounds.
    const auto results = stream.result_words.read(stream.segments * 3U);
    const auto slots = stream.slots.read(stream.slot_bytes);
    for (std::uint32_t segment = 0U; segment < stream.segments; ++segment) {
        const auto slot_begin = 2U * (segment << stream.shift) + 4U * segment;
        for (auto i = slot_begin; i < results[segment * 3U]; ++i) {
            REQUIRE_EQ(slots[i], CountDeviceBuffer<std::byte>::guard);
        }
    }
}
}  // namespace
#endif

TEST_CASE(gpu_rans_streams_singleton_states_match_independent_fixed_bytes) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const std::vector<std::byte> source(513U, std::byte{0xFF});
    constexpr std::array state{std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0}};
    for (const std::uint32_t shift : {8U, 12U}) {
        RansStreamFixture stream(source, shift);
        RansStatistics statistics({stream.input.data(), stream.bytes}, &stream.telemetry);
        for (const std::uint32_t bits : {0U, 8U}) {
            RansModelFixture model(1U << bits, stream.bytes);
            for (const std::uint32_t precision : {8U, 16U}) {
                prepare_stream_models(statistics, model, bits, shift, precision);
                require_stream_roundtrip(stream, model, source, precision, bits);
                const auto encoded = stream.output.read(stream.segments * 4U);
                for (std::uint32_t i = 0U; i < stream.segments; ++i) {
                    REQUIRE_TRUE(std::ranges::equal(std::span(encoded).subspan(i * 4U, 4U), state));
                }
            }
            model.release_checked();
        }
        statistics.close_checked();
        stream.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_streams_device_counts_models_and_bytes_cover_every_admitted_option) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    // This one fixed correctness input crosses the 128-segment dispatch at
    // shift 8, retains a partial tail at every shift, and includes a rare symbol.
    const auto source = rans_stream_source(33025U);
    for (std::uint32_t shift = 8U; shift <= 12U; ++shift) {
        RansStreamFixture stream(source, shift);
        RansStatistics statistics({stream.input.data(), stream.bytes}, &stream.telemetry);
        for (std::uint32_t bits = 0U; bits <= 8U; ++bits) {
            RansModelFixture model(1U << bits, stream.bytes);
            model.copy_counts(statistics.project(bits, shift));
            model.prepare();
            REQUIRE_TRUE(rans_model_rows_admitted(model.read_rows(), stream.bytes, bits));
            for (std::uint32_t precision = 8U; precision <= 16U; ++precision) {
                model.normalize(precision);
                model.require_status(RansModelStatus::success);
                model.build(precision);
                model.require_status(RansModelStatus::success);
                require_stream_roundtrip(stream, model, source, precision, bits);
            }
            model.release_checked();
        }
        REQUIRE_TRUE(snapshot_gpu_telemetry(stream.telemetry).kernel_launches > 0U);
        statistics.close_checked();
        stream.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_streams_invalid_plans_and_corrupted_models_reject_then_recover) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto source = rans_stream_source(33025U);
    RansStreamFixture stream(source, 8U);
    RansStatistics statistics({stream.input.data(), stream.bytes}, &stream.telemetry);
    RansModelFixture model(1U, stream.bytes);
    prepare_stream_models(statistics, model, 0U, 8U, 16U);
    struct Invalid {
        std::uint32_t precision, bits, shift, first, count, model_words, output_bytes;
    };
    const std::array invalid{Invalid{7U, 0U, 8U, 0U, 1U, 258U, stream.slot_bytes},
                             Invalid{17U, 0U, 8U, 0U, 1U, 258U, stream.slot_bytes},
                             Invalid{16U, 9U, 8U, 0U, 1U, 258U, stream.slot_bytes},
                             Invalid{16U, 0U, 13U, 0U, 1U, 258U, stream.slot_bytes},
                             Invalid{16U, 0U, 8U, 0U, 129U, 258U, stream.slot_bytes},
                             Invalid{16U, 0U, 8U, 0U, 1U, 257U, stream.slot_bytes},
                             Invalid{16U, 0U, 8U, 0U, 1U, 258U, stream.slot_bytes - 1U},
                             Invalid{16U, 0U, 8U, 0xFFFFFFFFU, 1U, 258U, stream.slot_bytes}};
    for (const auto value : invalid) {
        stream.result_words.reset({});
        stream.slots.reset({});
        dispatch_codec_kernel(&stream.telemetry, hip_kernel_api().encode_rans_segments, (value.count + 63U) / 64U, 64U,
                              "reject rANS encoder plan", stream.input.data(), stream.bytes,
                              static_cast<const std::uint32_t*>(model.models.data()), value.model_words,
                              static_cast<const std::uint32_t*>(model.reciprocals.data()), model.words, value.precision,
                              value.bits, value.shift, value.first, value.count, stream.slots.data(),
                              value.output_bytes, stream.result_data(), stream.segments);
        const auto results = stream.result_words.read(stream.segments * 3U);
        for (std::uint32_t segment = 0U; segment < stream.segments; ++segment) {
            const auto active = value.first != 0xFFFFFFFFU && segment < value.count;
            REQUIRE_EQ(results[segment * 3U + 2U], active ? static_cast<std::uint32_t>(RansModelStatus::invalid_plan)
                                                          : CountDeviceBuffer<std::uint32_t>::guard);
        }
        REQUIRE_TRUE(stream.slots.read(0U).empty());
    }
    const auto valid_models = model.models.read(258U);
    auto broken_models = valid_models;
    broken_models[0U] = 0U;
    model.models.reset(broken_models);
    stream.encode(model, 16U, 0U);
    const auto invalid_results = stream.result_words.read(stream.segments * 3U);
    for (std::uint32_t segment = 0U; segment < stream.segments; ++segment) {
        REQUIRE_EQ(invalid_results[segment * 3U + 2U], static_cast<std::uint32_t>(RansModelStatus::invalid_model));
    }
    REQUIRE_TRUE(stream.slots.read(0U).empty());
    model.models.reset(valid_models);
    model.reciprocals.fill(0U);
    stream.encode(model, 16U, 0U);
    const auto reciprocal_results = stream.result_words.read(stream.segments * 3U);
    for (std::uint32_t segment = 0U; segment < stream.segments; ++segment) {
        REQUIRE_EQ(reciprocal_results[segment * 3U + 2U], static_cast<std::uint32_t>(RansModelStatus::invalid_model));
    }
    model.build(16U);
    model.require_status(RansModelStatus::success);
    require_stream_roundtrip(stream, model, source, 16U, 0U);
    model.release_checked();
    statistics.close_checked();
    stream.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_streams_compaction_rejects_corrupt_ranges_before_dense_publication) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto source = rans_stream_source(1025U);
    RansStreamFixture stream(source, 8U);
    RansStatistics statistics({stream.input.data(), stream.bytes}, &stream.telemetry);
    RansModelFixture model(1U, stream.bytes);
    prepare_stream_models(statistics, model, 0U, 8U, 12U);
    stream.encode(model, 12U, 0U);
    const auto dense = stream.admit_stream_offsets();
    const auto original_results = stream.result_words.read(stream.segments * 3U);
    auto bad_offsets = dense;
    bad_offsets.back() += 1U;
    stream.offsets.reset(bad_offsets);
    stream.output.reset({});
    stream.statuses.reset(std::vector<RansModelStatus>(stream.segments, RansModelStatus::invalid_plan));
    dispatch_codec_kernel(&stream.telemetry, hip_kernel_api().compact_rans_segments, stream.segments + 1U, 256U,
                          "reject inconsistent rANS dense endpoint", static_cast<const std::byte*>(stream.slots.data()),
                          stream.slot_bytes, stream.bytes, stream.shift,
                          static_cast<const RansEncodeResult*>(stream.result_data()), stream.segments,
                          static_cast<const std::uint32_t*>(stream.offsets.data()), stream.segments + 1U, 0U,
                          stream.segments, stream.output.data(), dense.back(), stream.statuses.data(), stream.segments);
    REQUIRE_EQ(stream.statuses.read(stream.segments),
               std::vector<RansModelStatus>(stream.segments, RansModelStatus::invalid_model));
    REQUIRE_TRUE(stream.output.read(0U).empty());
    stream.offsets.reset(dense);
    for (const std::uint32_t field : {0U, 1U, 2U}) {
        auto corrupted = original_results;
        corrupted[3U + field] = 0xFFFFFFFFU;
        stream.result_words.reset(corrupted);
        stream.output.reset({});
        stream.statuses.reset(std::vector<RansModelStatus>(stream.segments, RansModelStatus::invalid_plan));
        dispatch_codec_kernel(
            &stream.telemetry, hip_kernel_api().compact_rans_segments, stream.segments + 1U, 256U,
            "reject one corrupt rANS source range", static_cast<const std::byte*>(stream.slots.data()),
            stream.slot_bytes, stream.bytes, stream.shift, static_cast<const RansEncodeResult*>(stream.result_data()),
            stream.segments, static_cast<const std::uint32_t*>(stream.offsets.data()), stream.segments + 1U, 0U,
            stream.segments, stream.output.data(), dense.back(), stream.statuses.data(), stream.segments);
        const auto statuses = stream.statuses.read(stream.segments);
        auto expected = std::vector<RansModelStatus>(stream.segments, RansModelStatus::success);
        expected[1U] = RansModelStatus::invalid_model;
        REQUIRE_EQ(statuses, expected);
        const auto output = stream.output.read(dense.back());
        REQUIRE_TRUE(std::ranges::all_of(std::span(output).subspan(dense[1U], dense[2U] - dense[1U]),
                                         [](auto byte) { return byte == CountDeviceBuffer<std::byte>::guard; }));
    }
    stream.result_words.reset(original_results);
    stream.compact(dense.back());
    stream.require_device_decode(model, dense, 12U, 0U, source);
    require_portable_rans_readback(rans_device_payload(model, stream, dense, 12U, 0U), source);
    model.release_checked();
    statistics.close_checked();
    stream.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_streams_maximum_source_bound_and_multiple_dispatches) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const std::vector<std::byte> source(kRansCountMaxBytes, std::byte{0xFF});
    RansStreamFixture stream(source, 12U);
    RansStatistics statistics({stream.input.data(), stream.bytes}, &stream.telemetry);
    RansModelFixture model(256U, stream.bytes);
    prepare_stream_models(statistics, model, 8U, 12U, 16U);
    require_stream_roundtrip(stream, model, source, 16U, 8U);
    REQUIRE_TRUE(stream.segments > kRansEncodeBatchSegments);
    model.release_checked();
    statistics.close_checked();
    stream.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
