#include "test_util.hpp"

#include "gpu/entropy_sampling.hpp"

#include <numeric>

namespace {

// Purpose: Independently count every input byte without the production tile permutation.
// Inputs: A bounded immutable byte span. Outputs: Exact frequencies for comparison with complete sampling.
std::array<std::uint64_t, 256> serial_histogram(std::span<const std::byte> bytes) {
    std::array<std::uint64_t, 256> histogram{};
    for (const auto byte : bytes) {
        ++histogram[std::to_integer<unsigned char>(byte)];
    }
    return histogram;
}

// Purpose: Require invalid requests to fail with the sampling API's documented exception.
// Inputs: A callable that must reject its input. Outputs: Fails the test if no GpuError is thrown.
template <typename Callable> void require_sampling_rejection(Callable&& call) {
    bool rejected = false;
    try {
        call();
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Detect skipped or duplicated tail bytes across incomplete permutation domains.
// Inputs: Every size through 4 KiB plus one, with nonuniform deterministic byte frequencies.
// Outputs: Requires each complete histogram and its total to equal an independent serial count.
TEST_CASE(entropy_sampling_complete_histogram_matches_every_small_extent) {
    std::vector<std::byte> bytes(4097U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>((index * 37U + index / 127U) & 255U);
    }
    for (std::size_t size = 1; size <= bytes.size(); ++size) {
        const auto input = std::span(bytes).first(size);
        flitzzip::EntropyHistogramSampler sampler(input);
        const auto expected = serial_histogram(input);
        REQUIRE_EQ(sampler.sample_to(size), expected);
        REQUIRE_EQ(sampler.sample_to(size), expected);
    }
}

// Purpose: Make omissions and duplicate visits observable even when most input bytes are identical.
// Inputs: A single marked byte at every position of an irregular 257-byte block.
// Outputs: Requires exactly one mark and 256 unmarked bytes for every position, including the final tail.
TEST_CASE(entropy_sampling_preserves_each_impulse_position) {
    std::array<std::byte, 257> bytes{};
    for (std::size_t marked = 0; marked < bytes.size(); ++marked) {
        bytes[marked] = std::byte{1};
        flitzzip::EntropyHistogramSampler sampler(bytes);
        const auto& histogram = sampler.sample_to(bytes.size());
        REQUIRE_EQ(histogram[0], bytes.size() - 1U);
        REQUIRE_EQ(histogram[1], 1U);
        bytes[marked] = std::byte{0};
    }
}

// Purpose: Prove incremental requests preserve the same nested sample and do not revisit prior bytes.
// Inputs: Irregular blocks spanning several strata, single-byte/tile/effort-sized requests, and direct prefixes.
// Outputs: Requires monotonic frequencies, exact totals, partition independence, and complete serial equality.
TEST_CASE(entropy_sampling_nested_prefixes_are_partition_independent) {
    for (const auto size : {65U, 1023U, 16385U, 65537U, 1048593U}) {
        std::vector<std::byte> bytes(size);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>((index * 13U + index / 65521U) & 255U);
        }
        flitzzip::EntropyHistogramSampler incremental(bytes);
        std::array<std::uint64_t, 256> previous{};
        for (const auto target :
             {std::size_t{1}, std::size_t{63}, std::size_t{64}, bytes.size() / 2U, bytes.size() - 1U, bytes.size()}) {
            const auto previous_total = std::accumulate(previous.begin(), previous.end(), std::uint64_t{0});
            if (target < previous_total) {
                continue;
            }
            const auto current = incremental.sample_to(target);
            flitzzip::EntropyHistogramSampler direct(bytes);
            REQUIRE_EQ(current, direct.sample_to(target));
            REQUIRE_EQ(std::accumulate(current.begin(), current.end(), std::uint64_t{0}), target);
            for (std::size_t symbol = 0; symbol < current.size(); ++symbol) {
                REQUIRE_TRUE(current[symbol] >= previous[symbol]);
            }
            previous = current;
        }
        REQUIRE_EQ(previous, serial_histogram(bytes));
    }
}

// Purpose: Catch prefix bias that would miss different distributions in distant source strata.
// Inputs: Eight 16 KiB regions with distinct byte values and only eight cache lines of sampling.
// Outputs: Requires equal representation from every region without completing the full histogram.
TEST_CASE(entropy_sampling_initial_tiles_cover_distant_strata) {
    std::vector<std::byte> bytes(8U * 16384U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index / 16384U);
    }
    flitzzip::EntropyHistogramSampler sampler(bytes);
    const auto& histogram = sampler.sample_to(8U * 64U);
    for (std::size_t region = 0; region < 8U; ++region) {
        REQUIRE_EQ(histogram[region], 64U);
    }
}

// Purpose: Ensure rejected sampling requests cannot poison valid subsequent use.
// Inputs: Empty source, zero/descending/oversized targets, and a nonempty source with one partial sample.
// Outputs: Requires documented failures, unchanged prefixes, and the correct complete histogram after rejection.
TEST_CASE(entropy_sampling_rejection_preserves_state) {
    require_sampling_rejection([] { flitzzip::EntropyHistogramSampler sampler({}); });
    const std::array<std::byte, 67> bytes{std::byte{7}, std::byte{11}, std::byte{13}};
    flitzzip::EntropyHistogramSampler sampler(bytes);
    require_sampling_rejection([&] { (void)sampler.sample_to(0); });
    const auto partial = sampler.sample_to(17U);
    for (const auto invalid : {0U, 1U, 16U, 68U}) {
        require_sampling_rejection([&] { (void)sampler.sample_to(invalid); });
        REQUIRE_EQ(sampler.sample_to(17U), partial);
    }
    REQUIRE_EQ(sampler.sample_to(bytes.size()), serial_histogram(bytes));
}

// Purpose: Keep effort budgets bounded and monotonic, with maximum effort inspecting the entire block.
// Inputs: Tiny and boundary-sized blocks, the largest supported extent, and invalid efforts/sizes.
// Outputs: Requires sampling invariants without allocating maximum-sized blocks or forming invalid spans.
TEST_CASE(entropy_sampling_effort_budget_contract) {
    for (const auto size : {std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{65537},
                            static_cast<std::size_t>(flitzzip::kMaxArchiveBlockBytes)}) {
        std::size_t previous = 0;
        for (int effort = 2; effort <= 9; ++effort) {
            const auto count = flitzzip::entropy_sample_count(size, effort);
            REQUIRE_TRUE(count >= 1U && count <= size && count >= previous);
            if (effort < 9 && size > 1U) {
                REQUIRE_TRUE(count < size);
            }
            previous = count;
        }
        REQUIRE_EQ(previous, size);
    }
    for (const auto effort : {-1, 0, 1, 10}) {
        require_sampling_rejection([&] { (void)flitzzip::entropy_sample_count(64U, effort); });
    }
    require_sampling_rejection([] { (void)flitzzip::entropy_sample_count(0U, 9); });
    require_sampling_rejection([] {
        (void)flitzzip::entropy_sample_count(static_cast<std::size_t>(flitzzip::kMaxArchiveBlockBytes) + 1U, 9);
    });
}
