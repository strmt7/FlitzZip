#include "cli/statistics_output.hpp"
#include "cli/usage_output.hpp"
#include "cli/memory_benchmark.hpp"
#include "stdout_capture.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>

namespace {

using namespace flitzzip;
using namespace flitzzip::cli;

constexpr std::string_view kGpuFields =
    " gpu_used=true gpu_encode_chunks=3 gpu_decode_chunks=5 gpu_kernel_launches=7 gpu_kernel_ms=0.125"
    " gpu_h2d_bytes=11 gpu_d2h_bytes=13 gpu_device_allocation_bytes=17 gpu_host_pinned_allocation_bytes=19"
    " gpu_host_pinned_output_bytes=23 gpu_pattern_blocks=29 gpu_prefix_blocks=31 gpu_dictionary_blocks=37"
    " gpu_sparse_pattern_blocks=41";

// Purpose: Compare computed finite statistics at a precision tighter than their serialized representation.
// Inputs: Actual and independently expected values. Outputs: Rejects nonfinite values or material arithmetic drift.
void require_close(double actual, double expected) {
    REQUIRE_TRUE(std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= 1e-12);
}

// Purpose: Keep the exact zero-result contract for empty input and invalid elapsed time.
// Inputs: One computed statistic. Outputs: Rejects nonzero values, including tiny residuals and nonfinite values.
void require_zero(double actual) {
    REQUIRE_EQ(std::fpclassify(actual), FP_ZERO);
}

// Purpose: Distinguish every projected counter without presenting component fixtures as measured GPU performance.
// Inputs: None. Outputs: Owned, deliberately nonuniform serialization values; no codec or device work executes.
OperationStats statistics_fixture() {
    OperationStats stats;
    stats.entries = 2;
    stats.input_bytes = 4194304;
    stats.output_bytes = 1048576;
    stats.workers = 3;
    stats.inflight_chunks = 2;
    stats.gpu_used = true;
    auto& gpu = stats.gpu_runtime;
    gpu.encode_chunks = 3;
    gpu.decode_chunks = 5;
    gpu.kernel_launches = 7;
    gpu.kernel_ms = 0.125;
    gpu.h2d_bytes = 11;
    gpu.d2h_bytes = 13;
    gpu.device_allocation_bytes = 17;
    gpu.host_pinned_allocation_bytes = 19;
    gpu.host_pinned_output_bytes = 23;
    gpu.pattern_blocks = 29;
    gpu.prefix_blocks = 31;
    gpu.dictionary_blocks = 37;
    gpu.sparse_pattern_blocks = 41;
    stats.seconds = 2;
    return stats;
}

// Purpose: Specify the entire ordinary record independently of production stream composition.
// Inputs: None. Outputs: Literal expected keys, order, ratios and terminator for the component fixture.
std::string expected_operation_record() {
    return "entries=2 input_bytes=4194304 output_bytes=1048576 workers=3 inflight_chunks=2" + std::string(kGpuFields) +
           " seconds=2 throughput_mib_s=2 compression_ratio=0.25\n";
}

// Purpose: Check machine-readable fields without using the producer's projection or formatting helpers.
// Inputs: One complete native record. Outputs: Unique nonempty key/value pairs; malformed framing fails.
std::map<std::string, std::string> parse_fields(const std::string& text) {
    REQUIRE_TRUE(!text.empty() && text.back() == '\n');
    REQUIRE_EQ(std::count(text.begin(), text.end(), '\n'), 1);
    std::map<std::string, std::string> result;
    std::istringstream input(text);
    for (std::string word; input >> word;) {
        const auto split = word.find('=');
        REQUIRE_TRUE(split != std::string::npos && split > 0 && split + 1 < word.size());
        REQUIRE_TRUE(result.emplace(word.substr(0, split), word.substr(split + 1)).second);
    }
    return result;
}

// Purpose: Inject bounded real ostream refusal without filesystem writes or resource exhaustion.
// Inputs: Maximum accepted byte count. Outputs: Exact accepted prefix and ordinary stream error propagation.
class LimitedOutput final : public std::streambuf {
  public:
    std::string bytes;

    // Purpose: Bound the sink. Inputs: Capacity in bytes. Outputs: Empty owned output.
    explicit LimitedOutput(std::size_t capacity) : capacity_(capacity) {}

  protected:
    // Purpose: Admit only the remaining prefix. Inputs: Valid borrowed bytes/count. Outputs: Actual written count.
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        const auto take = std::min(capacity_ - bytes.size(), static_cast<std::size_t>(count));
        bytes.append(data, take);
        return static_cast<std::streamsize>(take);
    }

    // Purpose: Model single-character failure consistently with bulk output.
    // Inputs: Character or EOF query. Outputs: Accepted character, or EOF when the fixed sink is full.
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        if (bytes.size() == capacity_) {
            return traits_type::eof();
        }
        bytes.push_back(traits_type::to_char_type(value));
        return value;
    }

  private:
    std::size_t capacity_;
};

}  // namespace

// Purpose: Keep the shared arithmetic's established empty-input and elapsed-time boundaries.
// Inputs: Independently computed MiB examples and zero/nonpositive time. Outputs: Exact throughput and ratio values.
TEST_CASE(cli_statistics_shared_units_and_zero_boundaries) {
    require_close(mib_per_second(4194304, 2), 2.0);
    require_close(mib_per_second(1048576, 0.25), 4.0);
    require_zero(mib_per_second(0, 2));
    require_zero(mib_per_second(4194304, 0));
    require_zero(mib_per_second(4194304, -1));
    require_close(compression_ratio(4194304, 1048576), 0.25);
    require_zero(compression_ratio(0, 1048576));
    require_zero(compression_ratio(4194304, 0));
}

// Purpose: Preserve the tuner's actual weighted-score consumer after sharing its arithmetic with ordinary output.
// Inputs: Distinct component phase times and complete bytes, never a live benchmark or performance claim.
// Outputs: Independently computed score, complete-container ratio and unchanged tuning geometry.
TEST_CASE(cli_statistics_benchmark_observation_preserves_score_and_complete_size) {
    MemoryBenchmarkResult result;
    result.stats = statistics_fixture();
    result.archive_bytes = 1049600;
    result.compress_seconds = 4;
    result.verify_seconds = 2;
    result.extract_seconds = 1;
    result.compression_level = 7;
    result.block_size = 262144;
    const auto observation = make_benchmark_suite_observation(result);
    require_close(observation.gpu_score, 20.0);
    require_close(observation.compression_ratio, 0.250244140625);
    REQUIRE_EQ(observation.compression_level, 7);
    REQUIRE_EQ(observation.block_size, 262144U);
    result.stats.input_bytes = 0;
    result.compress_seconds = result.verify_seconds = result.extract_seconds = 0;
    const auto empty = make_benchmark_suite_observation(result);
    require_zero(empty.gpu_score);
    require_zero(empty.compression_ratio);
}

// Purpose: Detect swapped counters, renamed keys, changed order or invented terminators in the common projection.
// Inputs: Distinct component counters. Outputs: Complete literal equality, with no device activity claimed.
TEST_CASE(cli_statistics_gpu_counters_match_independent_literal_order) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const auto stats = statistics_fixture();
    write_gpu_runtime_stats(output, stats.gpu_runtime, stats.gpu_used);
    REQUIRE_EQ(output.str(), kGpuFields);
}

// Purpose: Preserve the caller's GPU-use identity independently of individual counter values.
// Inputs: False identity and nonzero component counters. Outputs: Only the identity field changes.
TEST_CASE(cli_statistics_gpu_use_flag_is_not_inferred_from_counters) {
    std::ostringstream output;
    auto expected = std::string(kGpuFields);
    expected.replace(expected.find("true"), 4, "false");
    write_gpu_runtime_stats(output, statistics_fixture().gpu_runtime, false);
    REQUIRE_EQ(output.str(), expected);
}

// Purpose: Retain the complete ordinary CLI record through shared serializer extraction.
// Inputs: Distinct counters without optional phases. Outputs: Exact labels/order/derived values and one newline.
TEST_CASE(cli_statistics_operation_record_preserves_exact_wire_text) {
    std::ostringstream output;
    print_operation_stats(output, statistics_fixture());
    REQUIRE_EQ(output.str(), expected_operation_record());
}

// Purpose: Optional phase presence must not be confused with a positive-duration test.
// Inputs: Present zero phases and independently distinct fractional phases. Outputs: Exact optional suffixes.
TEST_CASE(cli_statistics_optional_phases_preserve_presence_and_mapping) {
    const std::array cases{
        std::pair{OperationPhaseStats{}, " setup_seconds=0 stream_seconds=0 publication_seconds=0\n"},
        std::pair{OperationPhaseStats{0.125, 0.25, 0.5},
                  " setup_seconds=0.125 stream_seconds=0.25 publication_seconds=0.5\n"}};
    for (const auto& [phases, suffix] : cases) {
        auto stats = statistics_fixture();
        stats.phases = phases;
        auto expected = expected_operation_record();
        expected.pop_back();
        expected += suffix;
        std::ostringstream output;
        print_operation_stats(output, stats);
        REQUIRE_EQ(output.str(), expected);
    }
}

// Purpose: Keep RAM benchmark projection consistent with ordinary counters and complete-container accounting.
// Inputs: Component values only; this test never runs a benchmark or reports them as real measurements.
// Outputs: Unique correctly mapped fields, independent stage values, separate ratios and RAM-only protocol tags.
TEST_CASE(cli_statistics_memory_record_preserves_counter_and_stage_mapping) {
    MemoryBenchmarkResult result;
    result.stats = statistics_fixture();
    result.archive_bytes = 1049600;
    result.validated_bytes = 4194304;
    result.codec_workers = 2;
    result.decode_inflight_chunks = 3;
    result.decode_codec_workers = 4;
    result.compress_seconds = 4;
    result.verify_seconds = 2;
    result.extract_seconds = 1;
    auto& gpu = result.stats.gpu_runtime;
    for (std::size_t index = 0; index < gpu.encode_stage_worker_seconds.size(); ++index) {
        gpu.encode_stage_worker_seconds[index] = static_cast<double>(101 + index);
    }
    for (std::size_t index = 0; index < gpu.classification_stage_worker_seconds.size(); ++index) {
        gpu.classification_stage_worker_seconds[index] = static_cast<double>(201 + index);
    }
    for (std::size_t index = 0; index < gpu.owned_decode_stage_worker_seconds.size(); ++index) {
        gpu.owned_decode_stage_worker_seconds[index] = static_cast<double>(301 + index);
    }
    flitzzip_test::StdoutCapture capture;
    print_memory_benchmark_stats(result);
    const auto text = capture.text();
    REQUIRE_TRUE(text.find(kGpuFields) != std::string::npos);
    const auto fields = parse_fields(text);
    const std::map<std::string, std::string> expected{{"gpu_readiness_worker_seconds", "101"},
                                                      {"gpu_analysis_worker_seconds", "102"},
                                                      {"gpu_classification_worker_seconds", "103"},
                                                      {"gpu_prefix_worker_seconds", "104"},
                                                      {"gpu_sparse_worker_seconds", "105"},
                                                      {"gpu_dictionary_worker_seconds", "106"},
                                                      {"gpu_publication_worker_seconds", "107"},
                                                      {"gpu_classification_allocation_worker_seconds", "201"},
                                                      {"gpu_classification_upload_worker_seconds", "202"},
                                                      {"gpu_classification_crc_worker_seconds", "203"},
                                                      {"gpu_classification_validation_worker_seconds", "204"},
                                                      {"decode_allocation_worker_seconds", "301"},
                                                      {"decode_materialization_worker_seconds", "302"},
                                                      {"decode_crc_worker_seconds", "303"},
                                                      {"compress_mib_s", "1"},
                                                      {"verify_mib_s", "2"},
                                                      {"extract_mib_s", "4"},
                                                      {"compression_ratio", "0.25"},
                                                      {"archive_compression_ratio", "0.250244"},
                                                      {"memory_only", "true"},
                                                      {"disk_write_bytes", "0"},
                                                      {"measurement_protocol", "bytewise-regenerated-v2"},
                                                      {"validated_bytes", "4194304"},
                                                      {"validation_worker_limit", "3"},
                                                      {"codec_workers", "2"},
                                                      {"decode_inflight_chunks", "3"},
                                                      {"decode_codec_workers", "4"}};
    for (const auto& [key, value] : expected) {
        REQUIRE_EQ(fields.at(key), value);
    }
}

// Purpose: A preflight record must never masquerade as timing or device-execution evidence.
// Inputs: Generated and corpus-metadata plans. Outputs: Exact configuration fields and explicit unverified identity.
TEST_CASE(cli_statistics_plan_record_never_claims_runtime_measurement) {
    for (const bool corpus : {false, true}) {
        MemoryBenchmarkPlan plan;
        plan.input_bytes = 12345;
        plan.workers = 4;
        plan.inflight_chunks = 2;
        plan.codec_workers = 2;
        plan.decode_inflight_chunks = 1;
        plan.decode_codec_workers = 4;
        if (corpus) {
            plan.expected_source_sha256 = std::string(64, 'a');
            plan.compression_mode = NativeCompressionMode::NeutronStar;
        }
        flitzzip_test::StdoutCapture capture;
        print_memory_benchmark_plan(plan);
        const auto fields = parse_fields(capture.text());
        REQUIRE_EQ(fields.size(), 11U);
        REQUIRE_EQ(fields.at("plan_only"), "true");
        REQUIRE_EQ(fields.at("input_bytes"), "12345");
        REQUIRE_EQ(fields.at("source_identity_verified"), "false");
        REQUIRE_EQ(fields.at("compression_mode"), corpus ? "neutron_star" : "standard");
        REQUIRE_EQ(fields.at("data_source"), corpus ? "corpus-metadata" : "generated");
        REQUIRE_EQ(fields.at("expected_source_sha256"), corpus ? std::string(64, 'a') : "unavailable");
        REQUIRE_TRUE(!fields.contains("seconds") && !fields.contains("gpu_used") && !fields.contains("memory_only"));
    }
}

// Purpose: Retain the caller's exception policy, partial-write evidence and successful reuse after sink failure.
// Inputs: Fixed in-RAM sinks failing at distinct record positions. Outputs: Typed refusal, exact prefix and recovery.
TEST_CASE(cli_statistics_output_failure_preserves_prefix_and_recovers) {
    const auto expected = expected_operation_record();
    for (const auto capacity : {std::size_t(0), std::size_t(1), std::size_t(64), expected.size() - 1}) {
        LimitedOutput sink(capacity);
        std::ostream output(&sink);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        bool rejected = false;
        try {
            print_operation_stats(output, statistics_fixture());
        } catch (const std::ios_base::failure&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(sink.bytes, expected.substr(0, capacity));
    }
    std::ostringstream recovered;
    print_operation_stats(recovered, statistics_fixture());
    REQUIRE_EQ(recovered.str(), expected);
}

// Purpose: A nonthrowing stream must keep refusal visible rather than silently report complete output.
// Inputs: Zero-capacity in-memory sink with the standard exception mask. Outputs: Failed stream and no bytes.
TEST_CASE(cli_statistics_nonthrowing_stream_keeps_failure_visible) {
    LimitedOutput sink(0);
    std::ostream output(&sink);
    print_operation_stats(output, statistics_fixture());
    REQUIRE_TRUE(output.fail());
    REQUIRE_TRUE(sink.bytes.empty());
}

// Purpose: Keep the extracted presentation entry point complete without executing a command or querying a device.
// Inputs: Owned stream. Outputs: One branded help block, current public commands and preserved lossless/GPU flags.
TEST_CASE(cli_statistics_help_lists_public_commands_without_executing_them) {
    std::ostringstream output;
    print_usage(output);
    const auto text = output.str();
    REQUIRE_TRUE(text.starts_with("FlitzZip CLI\nUsage:\n"));
    REQUIRE_TRUE(text.ends_with('\n'));
    for (const auto required :
         {"flitzzip_cli --help", "flitzzip_cli --version", "flitzzip_cli gpu-info", "flitzzip_cli gpu-diagnostic",
          "flitzzip_cli dependency-check", "flitzzip_cli formats", "flitzzip_cli compress", "flitzzip_cli extract",
          "flitzzip_cli verify", "--neutron-star", "--require-gpu", "--force-cpu", "--compression-level <1-9>"}) {
        REQUIRE_TRUE(text.find(required) != std::string::npos);
    }
    REQUIRE_TRUE(text.find("FlitzZip CLI", 1) == std::string::npos);
}
