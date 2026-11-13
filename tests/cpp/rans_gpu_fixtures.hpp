#pragma once

#include "test_util.hpp"
#include "gpu/hip_kernel_api.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace flitzzip_test {
using flitzzip::hip_detail::copy_on_codec_stream;
using flitzzip::hip_detail::HipDeviceBuffer;

// Initialized guards surround the actual allocation and every inactive logical
// tail. Every copy and dispatch completes before its caller reads or releases it.
template <typename T> class CountDeviceBuffer {
  public:
    static constexpr T guard = static_cast<T>(sizeof(T) == 1U ? 0xA5U : 0xA5A5A5A5U);

    explicit CountDeviceBuffer(std::uint32_t words)
        : words_(words), storage_((static_cast<std::size_t>(words) + 2U) * sizeof(T), "rANS count fixture buffer") {
        reset({});
    }
    T* data() const {
        return storage_.get() + 1U;
    }
    void reset(std::span<const T> prefix) {
        REQUIRE_TRUE(prefix.size() <= words_);
        std::vector<T> host(static_cast<std::size_t>(words_) + 2U, guard);
        std::ranges::copy(prefix, host.begin() + 1U);
        REQUIRE_EQ(copy_on_codec_stream(storage_.get(), host.data(), host.size() * sizeof(T), hipMemcpyHostToDevice),
                   hipSuccess);
    }
    void fill(T value) {
        reset(std::vector<T>(words_, value));
    }
    std::vector<T> read(std::uint32_t active) const {
        REQUIRE_TRUE(active <= words_);
        std::vector<T> host(static_cast<std::size_t>(words_) + 2U);
        REQUIRE_EQ(copy_on_codec_stream(host.data(), storage_.get(), host.size() * sizeof(T), hipMemcpyDeviceToHost),
                   hipSuccess);
        REQUIRE_EQ(host.front(), guard);
        REQUIRE_EQ(host.back(), guard);
        REQUIRE_TRUE(std::ranges::all_of(std::span(host).subspan(active + 1U, words_ - active),
                                         [](T value) { return value == guard; }));
        return {host.begin() + 1U, host.begin() + active + 1U};
    }
    void release_checked() {
        storage_.reset_checked("release rANS count fixture buffer");
    }

  private:
    std::uint32_t words_;
    HipDeviceBuffer<T> storage_;
};

// Shared actual-device model workspace. Count inputs may come from fixed host
// oracles or a preceding resident statistics operation. All buffers have real
// outer guards; the reservation outlives every owned allocation.
class RansModelFixture {
  public:
    RansModelFixture(std::uint32_t context_count, std::uint32_t source_bytes)
        : contexts(context_count), bytes(source_bytes), words(contexts * 256U),
          reservation((3U * (std::size_t{words} + 2U) + 3U * contexts + 2U + 258U * contexts + 2U + contexts + 2U) *
                              sizeof(std::uint32_t) +
                          words + 2U,
                      "rANS model fixture"),
          counts(words), order(words), row_words(contexts * 3U), frequencies(words), models(contexts * 258U),
          reciprocals(words), statuses(contexts) {}

    flitzzip::hip_detail::RansModelRow* row_data() const {
        return reinterpret_cast<flitzzip::hip_detail::RansModelRow*>(row_words.data());
    }
    std::vector<flitzzip::hip_detail::RansModelRow> read_rows() const {
        const auto actual = row_words.read(contexts * 3U);
        std::vector<flitzzip::hip_detail::RansModelRow> rows;
        rows.reserve(contexts);
        for (std::uint32_t row = 0U; row < contexts; ++row) {
            rows.push_back({actual[row * 3U], actual[row * 3U + 1U],
                            static_cast<flitzzip::hip_detail::RansModelStatus>(actual[row * 3U + 2U])});
        }
        return rows;
    }
    void prepare() {
        flitzzip::hip_detail::dispatch_codec_kernel(&telemetry, flitzzip::hip_kernel_api().prepare_rans_models,
                                                    contexts, 256U, "prepare rANS model fixture", counts.data(), words,
                                                    bytes, order.data(), words, row_data(), contexts);
    }
    void normalize(std::uint32_t precision) {
        frequencies.reset({});
        statuses.fill(flitzzip::hip_detail::RansModelStatus::invalid_plan);
        flitzzip::hip_detail::dispatch_codec_kernel(
            &telemetry, flitzzip::hip_kernel_api().normalize_rans_models, contexts, 256U,
            "normalize rANS model fixture", counts.data(), words, order.data(), words,
            static_cast<const flitzzip::hip_detail::RansModelRow*>(row_data()), contexts, bytes, precision,
            frequencies.data(), words, statuses.data(), contexts);
    }
    void build(std::uint32_t precision) {
        models.reset({});
        reciprocals.reset({});
        statuses.fill(flitzzip::hip_detail::RansModelStatus::invalid_plan);
        flitzzip::hip_detail::dispatch_codec_kernel(
            &telemetry, flitzzip::hip_kernel_api().build_rans_models, contexts, 256U, "build rANS model fixture",
            static_cast<const std::uint32_t*>(frequencies.data()), words,
            static_cast<const flitzzip::hip_detail::RansModelRow*>(row_data()), contexts, precision, models.data(),
            contexts * 258U, reciprocals.data(), words, statuses.data(), contexts);
    }
    void require_status(flitzzip::hip_detail::RansModelStatus expected) const {
        REQUIRE_EQ(statuses.read(contexts), std::vector<flitzzip::hip_detail::RansModelStatus>(contexts, expected));
    }
    void copy_counts(std::span<const std::uint32_t> resident) {
        REQUIRE_EQ(resident.size(), words);
        REQUIRE_EQ(copy_on_codec_stream(counts.data(), resident.data(), resident.size_bytes(), hipMemcpyDeviceToDevice),
                   hipSuccess);
    }
    void release_checked() {
        statuses.release_checked();
        reciprocals.release_checked();
        models.release_checked();
        frequencies.release_checked();
        row_words.release_checked();
        order.release_checked();
        counts.release_checked();
        reservation.reduce_to(0U);
    }

    const std::uint32_t contexts, bytes, words;
    flitzzip::hip_detail::HipDeviceMemoryReservation reservation;
    CountDeviceBuffer<std::uint32_t> counts;
    CountDeviceBuffer<std::uint8_t> order;
    CountDeviceBuffer<std::uint32_t> row_words;
    CountDeviceBuffer<std::uint32_t> frequencies;
    CountDeviceBuffer<std::uint32_t> models;
    CountDeviceBuffer<std::uint32_t> reciprocals;
    CountDeviceBuffer<flitzzip::hip_detail::RansModelStatus> statuses;
    flitzzip::GpuTelemetry telemetry{flitzzip::GpuObservationMode::CountersOnly};
};

}  // namespace flitzzip_test
