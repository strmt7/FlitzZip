#include "gpu/dictionary_matcher.hpp"
#include "gpu/dictionary_candidate.hpp"
#include "gpu/gpu_codec.hpp"
#include "app/compression_mode_selection.hpp"
#include "core/result.hpp"
#include "core/progress.hpp"
#include "core/archive.hpp"
#include "core/compound_block.hpp"
#include "core/byte_plane_block.hpp"
#include "test_util.hpp"
#include "lz4.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace superzip::dictionary;

// Purpose: Reject the complete malformed layout before entering a GPU boundary or changing prior winners.
// Inputs: Live host storage and inconsistent descriptors; HIP builds also exercise the device dispatcher checkpoint.
// Outputs: Requires precise host-layout rejection in both builds and rejection before actual HIP work when compiled.
TEST_CASE(neutron_dictionary_layout_admission_precedes_device_work) {
    const std::array<std::byte, 4> input{};
    superzip::BlockDescriptor first{};
    first.uncompressed_len = 2U;
    first.encoded_len = 4096U;
    const auto require_rejection = [&](std::vector<superzip::BlockDescriptor> blocks, std::string_view expected) {
        DictionaryReplacements replacements(blocks.size());
        const auto original = replacements;
        superzip::GpuTelemetry telemetry;
        unsigned int checkpoints = 0U;
        bool rejected = false;
        try {
#if SUPERZIP_ENABLE_HIP
            improve_neutron_replacements(input, input.data(), blocks, replacements, &telemetry, [&] {
                ++checkpoints;
                throw std::runtime_error("Neutron device boundary must not be reached");
            });
#else
            validate_neutron_layout(input.size(), blocks, replacements);
#endif
        } catch (const superzip::GpuError& error) {
            rejected = std::string_view(error.what()) == expected;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(checkpoints, 0U);
        REQUIRE_EQ(replacements, original);
        REQUIRE_EQ(telemetry.kernel_launches.load(), 0U);
        REQUIRE_EQ(telemetry.device_allocation_bytes.load(), 0U);
    };
    require_rejection({first}, "Neutron replacement blocks do not cover their source chunk");
    auto invalid = first;
    invalid.uncompressed_len = 0U;
    require_rejection({first, invalid}, "Neutron replacement block exceeds its source chunk");
    invalid.uncompressed_len = std::numeric_limits<std::uint32_t>::max();
    require_rejection({first, invalid}, "Neutron replacement block exceeds its source chunk");
    validate_neutron_layout(input.size(), std::array{first, first}, DictionaryReplacements(2U));
    bool count_rejected = false;
    try {
        validate_neutron_layout(input.size(), std::array{first, first}, DictionaryReplacements(1U));
    } catch (const superzip::GpuError& error) {
        count_rejected = std::string_view(error.what()) == "Neutron replacement inputs are inconsistent";
    }
    REQUIRE_TRUE(count_rejected);
}

// Purpose: Keep Neutron separate and invisible unless native format, real HIP capability and required-GPU policy agree.
// Inputs: Every combination of the three admission prerequisites and an existing Neutron choice.
// Outputs: Requires nine unchanged numeric rows, one conditional Neutron row, and explicit unavailable normalization.
TEST_CASE(neutron_dictionary_gui_selection_isolation) {
    using namespace superzip::app;
    for (const bool native : {false, true}) {
        for (const bool available : {false, true}) {
            for (const bool required : {false, true}) {
                const bool eligible = native && available && required;
                REQUIRE_EQ(neutron_mode_eligible(native, available, required), eligible);
                REQUIRE_EQ(compression_selection_count(eligible), eligible ? 10 : 9);
                UiState state;
                state.gpu_available = available;
                state.gpu_required = required;
                state.compression_level_index = kNeutronCompressionLevelIndex;
                REQUIRE_EQ(normalize_neutron_selection(state, native), !eligible);
                REQUIRE_EQ(state.compression_level_index, eligible ? 9 : 8);
                if (!eligible) {
                    REQUIRE_TRUE(state.status.find("level 9 selected") != std::string::npos);
                }
                for (int level = 0; level < 9; ++level) {
                    state.compression_level_index = level;
                    REQUIRE_TRUE(!normalize_neutron_selection(state, native));
                    REQUIRE_EQ(state.compression_level_index, level);
                }
            }
        }
    }
}

// Purpose: Compute each longest exact earlier match independently of the GPU index and work schedule.
// Inputs: A small immutable segment with fewer predecessors than the Neutron search limit.
// Outputs: Returns all usable match lengths after independently enforcing LZ4's final-literal and start restrictions.
std::vector<std::size_t> exhaustive_matches(std::span<const std::byte> input) {
    std::vector<std::size_t> matches(input.size());
    for (std::size_t position = 1U; position + 12U <= input.size(); ++position) {
        const auto maximum = std::min<std::size_t>(kMaxNeutronMatchBytes, input.size() - position - 5U);
        for (auto earlier = position; earlier != 0U;) {
            --earlier;
            std::size_t length = 0U;
            while (length < maximum && input[earlier + length] == input[position + length]) {
                ++length;
            }
            matches[position] = std::max(matches[position], length >= 4U ? length : 0U);
            if (matches[position] == maximum) {
                break;
            }
        }
    }
    return matches;
}

// Purpose: Enumerate every legal literal-run and match-length transition as an independent byte-cost oracle.
// Inputs: A small immutable segment; no GPU tree, modulo-residue optimization or greedy parse is reused.
// Outputs: Returns the minimum complete LZ4 payload length for the exhaustive match graph.
std::size_t exhaustive_encoded_size(std::span<const std::byte> input) {
    const auto matches = exhaustive_matches(input);
    std::vector<std::size_t> suffix(input.size() + 1U, 1U);
    for (auto cursor = input.size(); cursor != 0U;) {
        --cursor;
        const auto tail = input.size() - cursor;
        suffix[cursor] = 1U + tail + (tail < 15U ? 0U : (tail - 15U) / 255U + 1U);
        for (auto next = cursor; next + 12U <= input.size(); ++next) {
            const auto literals = next - cursor;
            const auto literal_extension = literals < 15U ? 0U : (literals - 15U) / 255U + 1U;
            for (std::size_t length = 4U; length <= matches[next]; ++length) {
                const auto match_extension = length < 19U ? 0U : (length - 19U) / 255U + 1U;
                suffix[cursor] = std::min(suffix[cursor],
                                          3U + literals + literal_extension + match_extension + suffix[next + length]);
            }
        }
    }
    return suffix.front();
}

// Purpose: Check emitted segments against an independent library and the production HIP decoder.
// Inputs: Original bytes and completed GPU encoder output, including telemetry.
// Outputs: Requires exact recovery, source/segment extents, bounded memory and genuine device work.
void require_neutron_roundtrip(std::span<const std::byte> input, const EncodedBatch& encoded) {
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.h2d_bytes, input.size());
    REQUIRE_TRUE(encoded.device_workspace_bytes <= kMaxWorkspaceBytes);
    REQUIRE_TRUE(encoded.explicit_kernel_launches > 4U);
    std::size_t offset = 0U;
    for (const auto& segment : encoded.segments) {
        REQUIRE_TRUE(segment.input_bytes > 0U && segment.input_bytes <= kSegmentBytes);
        REQUIRE_TRUE(segment.payload.size() > 0U && segment.payload.size() <= kEncodedSegmentCapacity);
        std::vector<std::byte> decoded(segment.input_bytes);
        const auto bytes = LZ4_decompress_safe(
            reinterpret_cast<const char*>(segment.payload.data()), reinterpret_cast<char*>(decoded.data()),
            static_cast<int>(segment.payload.size()), static_cast<int>(segment.input_bytes));
        REQUIRE_EQ(bytes, static_cast<int>(segment.input_bytes));
        REQUIRE_TRUE(segment.input_bytes <= input.size() - offset);
        REQUIRE_TRUE(std::equal(decoded.begin(), decoded.end(), input.begin() + static_cast<std::ptrdiff_t>(offset)));
        offset += segment.input_bytes;
    }
    REQUIRE_EQ(offset, input.size());
    const auto decoded = decode_segments(encoded.segments);
    REQUIRE_TRUE(decoded.gpu_used);
    REQUIRE_TRUE(std::equal(decoded.bytes.begin(), decoded.bytes.end(), input.begin(), input.end()));
}

// Purpose: Generate deterministic small alphabets with overlapping matches and independently varied suffixes.
// Inputs: Exact size, integer seed and nonzero alphabet width.
// Outputs: Returns reproducible RAM-only source bytes, without a filesystem fixture.
std::vector<std::byte> small_neutron_fixture(std::size_t size, std::uint32_t seed, std::uint32_t alphabet) {
    std::vector<std::byte> input(size);
    for (auto& value : input) {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        value = static_cast<std::byte>(seed % alphabet);
    }
    return input;
}

}  // namespace

// Purpose: Retain a CPU-only oracle control whose optimum is known without the implementation under test.
// Inputs: Literal-only short inputs and one independently interpretable distance-one repetition.
// Outputs: Requires correct token, extension and final-tail accounting even on hosted systems without HIP.
TEST_CASE(neutron_dictionary_exhaustive_oracle_controls) {
    REQUIRE_EQ(exhaustive_encoded_size({}), 1U);
    REQUIRE_EQ(exhaustive_encoded_size(std::vector<std::byte>(12U, std::byte{0x61})), 13U);
    REQUIRE_EQ(exhaustive_encoded_size(std::vector<std::byte>(13U, std::byte{0x61})), 10U);
    auto literals = small_neutron_fixture(15U, 13U, 256U);
    REQUIRE_EQ(exhaustive_encoded_size(literals), 17U);
}

// Purpose: Reject overlarge work and propagate caller cancellation before any HIP admission.
// Inputs: Empty input, an oversized batch and a throwing first checkpoint.
// Outputs: Requires no GPU work for empty input and exact failure categories for invalid/cancelled requests.
TEST_CASE(neutron_dictionary_admission_and_unavailable_gpu) {
    REQUIRE_TRUE(!encode_neutron_segments({}).gpu_used);
    bool rejected = false;
    try {
        static_cast<void>(encode_neutron_segments(std::vector<std::byte>(kMaxNeutronBatchBytes + 1U)));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    bool cancelled = false;
    try {
        static_cast<void>(
            encode_neutron_segments({}, [] { throw std::runtime_error("Neutron control cancellation"); }));
    } catch (const std::runtime_error& error) {
        cancelled = std::string_view(error.what()) == "Neutron control cancellation";
    }
    REQUIRE_TRUE(cancelled);
    if (!superzip::query_gpu_info().available) {
        rejected = false;
        try {
            static_cast<void>(encode_neutron_segments(std::vector<std::byte>(17U)));
        } catch (const superzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Prevent direct API callers from obtaining a CPU or weaker-effort Neutron operation.
// Inputs: Invalid mode, CPU override, optional-GPU policy and weaker effort, including empty source.
// Outputs: Requires policy rejection before codec work even on hosted systems without HIP.
TEST_CASE(neutron_dictionary_native_policy_rejects_fallback) {
    for (const auto control : {0U, 1U, 2U, 3U}) {
        superzip::GpuCodecOptions options{.require_gpu = true,
                                          .force_cpu = false,
                                          .compression_level = 9,
                                          .compression_mode = superzip::NativeCompressionMode::NeutronStar};
        if (control == 0U) {
            options.force_cpu = true;
        } else if (control == 1U) {
            options.require_gpu = false;
        } else if (control == 2U) {
            options.compression_level = 8;
        } else {
            options.compression_mode = static_cast<superzip::NativeCompressionMode>(255U);
        }
        bool rejected = false;
        try {
            static_cast<void>(superzip::encode_chunk({}, options));
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Serialize observer copies across simultaneous workers and retain synchronous reentrant calls.
// Inputs: Two concurrent streams of snapshots and a callback that reenters its own serialized wrapper once.
// Outputs: Requires no observer overlap, exact delivery count and safe reentrancy without a held progress-state lock.
TEST_CASE(neutron_dictionary_progress_serialization) {
    std::atomic<unsigned int> active{0U};
    std::atomic<unsigned int> calls{0U};
    std::atomic<bool> overlap{false};
    const auto observer = superzip::serialize_progress_callback([&](const superzip::ProgressSnapshot&) {
        if (active.fetch_add(1U) != 0U) {
            overlap.store(true);
        }
        std::this_thread::yield();
        ++calls;
        --active;
    });
    std::latch ready{2};
    const auto worker = [&] {
        ready.count_down();
        ready.wait();
        for (unsigned int index = 0U; index < 64U; ++index) {
            observer({});
        }
    };
    auto first = std::async(std::launch::async, worker);
    auto second = std::async(std::launch::async, worker);
    first.get();
    second.get();
    REQUIRE_TRUE(!overlap.load());
    REQUIRE_EQ(calls.load(), 128U);
    unsigned int nested_calls = 0U;
    superzip::ProgressCallback reentrant;
    reentrant = superzip::serialize_progress_callback([&](const superzip::ProgressSnapshot& snapshot) {
        if (++nested_calls == 1U) {
            reentrant(snapshot);
        }
    });
    reentrant({});
    REQUIRE_EQ(nested_calls, 2U);
}

// Purpose: Stop every checkpoint copy after an observer failure without replacing the original exception.
// Inputs: Shared progress, one throwing observer and sibling checkpoints.
// Outputs: Requires original error propagation, shared cancellation and rejection before subsequent observer calls.
TEST_CASE(neutron_dictionary_progress_failure_cancels_siblings) {
    superzip::ProgressState progress;
    progress.start(superzip::OperationKind::Compress, 128U, 1U);
    unsigned int calls = 0U;
    auto checkpoint = superzip::make_cancellation_checkpoint(progress, [&](const superzip::ProgressSnapshot&) {
        ++calls;
        throw std::runtime_error("Neutron observer control");
    });
    auto sibling = checkpoint;
    bool original = false;
    try {
        checkpoint();
    } catch (const std::runtime_error& error) {
        original = std::string_view(error.what()) == "Neutron observer control";
    }
    REQUIRE_TRUE(original);
    REQUIRE_TRUE(progress.cancelled());
    bool cancelled = false;
    try {
        sibling();
    } catch (const superzip::ArchiveError&) {
        cancelled = true;
    }
    REQUIRE_TRUE(cancelled);
    REQUIRE_EQ(calls, 1U);
}

// Purpose: Preserve the complete ordinary GPU portfolio and CPU/HIP readability through the real chunk dispatcher.
// Inputs: Nonperiodic source with isolated repeats missed by the ordinary sampling heuristic, at the same level nine.
// Outputs: Requires a strictly smaller dictionary winner, complete CRC semantics and exact CPU/HIP decoding.
TEST_CASE(neutron_dictionary_native_portfolio_and_readback) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron native portfolio requires HIP\n";
        return;
    }
    auto input = small_neutron_fixture(600U, 713947U, 256U);
    std::copy_n(input.begin() + 4, 16U, input.begin() + 230);
    std::copy_n(input.begin() + 32, 20U, input.begin() + 512);
    const auto baseline = superzip::encode_chunk(input, {.require_gpu = true, .compression_level = 9});
    auto telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded =
        superzip::encode_chunk(input, {.require_gpu = true,
                                       .compression_level = 9,
                                       .compression_mode = superzip::NativeCompressionMode::NeutronStar,
                                       .telemetry = telemetry});
    REQUIRE_TRUE(encoded.payload.size() < baseline.payload.size());
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::GpuDictionary);
    REQUIRE_TRUE(encoded.gpu_used && encoded.source_crc32_available);
    REQUIRE_EQ(encoded.source_crc32, baseline.source_crc32);
    std::vector<std::byte> decoded(input.size());
    superzip::decode_chunk_cpu(encoded.payload, encoded.blocks, decoded, {});
    REQUIRE_EQ(decoded, input);
    REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, {.require_gpu = true}));
    REQUIRE_EQ(decoded, input);
    REQUIRE_TRUE(superzip::snapshot_gpu_telemetry(*telemetry).kernel_launches > 20U);
}

// Purpose: Verify the separate mode through real native archive publication, framing and both decoder policies.
// Inputs: Four small nonperiodic files with isolated repeats, one ordinary GPU archive and one Neutron archive.
// Outputs: Requires a smaller complete archive, required-HIP telemetry and exact restored bytes for every file.
TEST_CASE(neutron_dictionary_complete_archive_readback) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron archive publication requires HIP\n";
        return;
    }
    const auto root = test_temp_dir("neutron-archive");
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    auto input = small_neutron_fixture(600U, 713947U, 256U);
    std::copy_n(input.begin() + 4, 16U, input.begin() + 230);
    std::copy_n(input.begin() + 32, 20U, input.begin() + 512);
    const std::string expected(reinterpret_cast<const char*>(input.data()), input.size());
    for (int file = 0; file < 4; ++file) {
        std::ofstream stream(source / (std::to_string(file) + ".bin"), std::ios::binary);
        stream.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        REQUIRE_TRUE(stream.good());
    }
    superzip::CompressOptions options;
    options.compression_level = 9;
    options.gpu_required = true;
    const auto baseline_path = root / "standard.suzip";
    superzip::compress_suzip({source}, baseline_path, options);
    options.compression_mode = superzip::NativeCompressionMode::NeutronStar;
    options.verify_after_write = true;
    const auto neutron_path = root / "neutron.suzip";
    const auto stats = superzip::compress_suzip({source}, neutron_path, options);
    REQUIRE_TRUE(stats.gpu_used);
    REQUIRE_TRUE(stats.gpu_runtime.kernel_launches > 20U);
    REQUIRE_TRUE(std::filesystem::file_size(neutron_path) < std::filesystem::file_size(baseline_path));
    for (const bool gpu : {false, true}) {
        const auto destination = root / (gpu ? "hip" : "cpu");
        const auto extracted =
            superzip::extract_suzip(neutron_path, destination, {.gpu_required = gpu, .force_cpu = !gpu});
        REQUIRE_EQ(extracted.gpu_used, gpu);
        for (int file = 0; file < 4; ++file) {
            std::ifstream stream(destination / "source" / (std::to_string(file) + ".bin"), std::ios::binary);
            const std::string actual{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            REQUIRE_EQ(actual, expected);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve an existing destination when a Neutron progress observer cancels archive creation.
// Inputs: A bounded source, existing destination bytes and a deliberately failing progress observer.
// Outputs: Requires the original exception and unchanged destination without partial publication.
TEST_CASE(neutron_dictionary_archive_cancellation_preserves_destination) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron archive cancellation requires HIP\n";
        return;
    }
    const auto root = test_temp_dir("neutron-cancellation");
    const auto source = root / "input.bin";
    const auto output = root / "archive.suzip";
    const std::string sentinel = "existing destination control";
    std::ofstream(output, std::ios::binary) << sentinel;
    const std::string input(128U * 1024U, 'N');
    std::ofstream(source, std::ios::binary).write(input.data(), static_cast<std::streamsize>(input.size()));
    superzip::CompressOptions options;
    options.compression_level = 9;
    options.compression_mode = superzip::NativeCompressionMode::NeutronStar;
    bool cancelled = false;
    int publications = 0;
    try {
        superzip::compress_suzip({source}, output, options, [&](const superzip::ProgressSnapshot&) {
            if (++publications == 3) {
                throw std::runtime_error("Neutron archive cancellation control");
            }
        });
    } catch (const std::runtime_error& error) {
        cancelled = std::string_view(error.what()) == "Neutron archive cancellation control";
    }
    REQUIRE_TRUE(cancelled);
    std::ifstream stream(output, std::ios::binary);
    const std::string actual{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    REQUIRE_EQ(actual, sentinel);
    stream.close();
    std::filesystem::remove_all(root);
}

// Purpose: Cover extension plateaus across wavefronts, beyond the lane count and through the legal segment limit.
// Inputs: Uniform independent segments with closed-form minimum LZ4 representations.
// Outputs: Requires one long match with the exact minimum cost, independent LZ4 readback and production HIP readback.
TEST_CASE(neutron_dictionary_full_segment_match_extension) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron full-length match requires HIP\n";
        return;
    }
    for (const auto size : {600U, 16384U, 32768U, 50000U, 65048U, kSegmentBytes}) {
        const std::vector<std::byte> input(size, std::byte{0x31});
        const auto encoded = encode_neutron_segments(input);
        const auto match_length = input.size() - 1U - 5U;
        const auto match_extension = 1U + (match_length - 4U - 15U) / 255U;
        const auto minimum_bytes = 1U + 1U + 2U + match_extension + 1U + 5U;
        REQUIRE_EQ(encoded.segments.size(), 1U);
        REQUIRE_EQ(encoded.segments.front().payload.size(), minimum_bytes);
        require_neutron_roundtrip(input, encoded);
    }
}

// Purpose: Compare real HIP minimum-byte parsing against every legal transition in an independent small oracle.
// Inputs: Deterministic alphabets, overlaps, literal tails and extension boundaries, all RAM-only.
// Outputs: Requires exact minimum byte count, deterministic payloads and independent CPU/HIP recovery.
TEST_CASE(neutron_dictionary_gpu_matches_exhaustive_oracle) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron minimum-byte parsing requires a compatible HIP device\n";
        return;
    }
    for (const auto size : {1U, 4U, 12U, 13U, 15U, 19U, 32U, 64U, 127U, 254U, 255U, 269U, 270U, 320U}) {
        for (const auto alphabet : {1U, 2U, 16U, 256U}) {
            const auto input = small_neutron_fixture(size, size + 0x716255U, alphabet);
            const auto encoded = encode_neutron_segments(input);
            REQUIRE_EQ(encoded.segments.size(), 1U);
            REQUIRE_EQ(encoded.segments.front().payload.size(), exhaustive_encoded_size(input));
            require_neutron_roundtrip(input, encoded);
        }
    }
    // Two isolated repetitions force later suffixes across both sides of the modulo-255 carry boundary.
    for (const auto next : {511U, 512U, 519U, 520U, 524U}) {
        auto input = small_neutron_fixture(600U, 713947U, 256U);
        std::copy_n(input.begin() + 4, 16U, input.begin() + 230);
        std::copy_n(input.begin() + 32, 20U, input.begin() + next);
        const auto encoded = encode_neutron_segments(input);
        REQUIRE_EQ(encoded.segments.front().payload.size(), exhaustive_encoded_size(input));
        require_neutron_roundtrip(input, encoded);
    }
    const auto input = small_neutron_fixture(320U, 25517U, 4U);
    REQUIRE_EQ(encode_neutron_segments(input).segments.front().payload,
               encode_neutron_segments(input).segments.front().payload);
}

// Purpose: Generate a bounded de Bruijn cycle whose four-byte windows are unique inside one dictionary segment.
// Inputs: A four-digit base-16 recursion state with depth at most five and output bounded to 65,536 bytes.
// Outputs: Appends the exact cycle without deep recursion, random assumptions or product encoder reuse.
void append_unique_neutron_cycle(std::vector<std::byte>& output, std::array<std::uint32_t, 5>& digits,
                                 std::size_t depth, std::size_t period) {
    if (depth > 4U) {
        if (4U % period == 0U) {
            for (std::size_t index = 1U; index <= period; ++index) {
                output.push_back(static_cast<std::byte>(digits[index]));
            }
        }
        return;
    }
    digits[depth] = digits[depth - period];
    append_unique_neutron_cycle(output, digits, depth + 1U, period);
    for (auto symbol = digits[depth - period] + 1U; symbol < 16U; ++symbol) {
        digits[depth] = symbol;
        append_unique_neutron_cycle(output, digits, depth + 1U, depth);
    }
}

// Purpose: Authenticate the no-match fixture independently before using it to qualify GPU parse admission.
// Inputs: The bounded base-16 cycle generator and every complete four-byte source window.
// Outputs: Returns one segment only after sorting proves that no usable exact-prefix match can exist.
std::vector<std::byte> unique_neutron_segment() {
    std::vector<std::byte> output;
    output.reserve(kSegmentBytes);
    std::array<std::uint32_t, 5> digits{};
    append_unique_neutron_cycle(output, digits, 1U, 1U);
    REQUIRE_EQ(output.size(), kSegmentBytes);
    std::vector<std::uint32_t> words;
    words.reserve(output.size() - 3U);
    for (std::size_t position = 0U; position + 4U <= output.size(); ++position) {
        std::uint32_t word = 0U;
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            word = (word << 8U) | std::to_integer<std::uint32_t>(output[position + byte]);
        }
        words.push_back(word);
    }
    std::sort(words.begin(), words.end());
    REQUIRE_TRUE(std::adjacent_find(words.begin(), words.end()) == words.end());
    return output;
}

// Purpose: Prove exact empty match graphs avoid dynamic programming while still encoding and decoding on HIP.
// Inputs: Independently verified unique four-byte windows through the largest admitted batch and a mixed batch.
// Outputs: Requires exact literal costs, zero parse launches for empty graphs and only the active segment's tiles.
TEST_CASE(neutron_dictionary_empty_graph_and_mixed_admission) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron match-graph admission requires HIP\n";
        return;
    }
    const auto unique = unique_neutron_segment();
    for (const auto size : {std::size_t{13U}, std::size_t{600U}, std::size_t{kSegmentBytes}, kMaxNeutronBatchBytes}) {
        std::vector<std::byte> input(size);
        for (std::size_t position = 0U; position < size; ++position) {
            input[position] = unique[position % unique.size()];
        }
        const auto encoded = encode_neutron_segments(input);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.neutron_active_segment_mask, 0U);
        REQUIRE_EQ(encoded.neutron_parse_launches, 0U);
        REQUIRE_EQ(encoded.explicit_kernel_launches,
                   7U + (size + kNeutronSearchTileBytes - 1U) / kNeutronSearchTileBytes);
        for (const auto& segment : encoded.segments) {
            const auto bytes = segment.input_bytes;
            const auto extension = bytes < 15U ? 0U : 1U + (bytes - 15U) / 255U;
            REQUIRE_EQ(segment.payload.size(), 1U + bytes + extension);
        }
        require_neutron_roundtrip(input, encoded);
    }
    auto mixed = unique;
    const std::vector<std::byte> repeated(600U, std::byte{0xFE});
    mixed.insert(mixed.end(), repeated.begin(), repeated.end());
    const auto encoded = encode_neutron_segments(mixed);
    REQUIRE_EQ(encoded.neutron_active_segment_mask, 2U);
    REQUIRE_EQ(encoded.neutron_parse_launches,
               (repeated.size() + kNeutronParseTilePositions - 1U) / kNeutronParseTilePositions);
    REQUIRE_EQ(encoded.segments.at(1).payload, encode_neutron_segments(repeated).segments.front().payload);
    require_neutron_roundtrip(mixed, encoded);
}

// Purpose: Exercise cancellation during different synchronized stages and prove subsequent GPU work remains usable.
// Inputs: Throwing checkpoints before admission, after initialization and during descending parse tiles.
// Outputs: Requires exact cancellation propagation and successful byte-exact encoding after every interrupted run.
TEST_CASE(neutron_dictionary_cancellation_releases_workspace) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron staged cancellation requires HIP\n";
        return;
    }
    const auto input = small_neutron_fixture(128U, 7139U, 4U);
    for (const auto stop : {1U, 3U, 6U, 7U}) {
        std::uint32_t checkpoints = 0U;
        bool cancelled = false;
        try {
            static_cast<void>(encode_neutron_segments(input, [&] {
                if (++checkpoints == stop) {
                    throw std::runtime_error("Neutron staged cancellation");
                }
            }));
        } catch (const std::runtime_error& error) {
            cancelled = std::string_view(error.what()) == "Neutron staged cancellation";
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(checkpoints, stop);
        const auto encoded = encode_neutron_segments(input);
        REQUIRE_EQ(encoded.segments.front().payload.size(), exhaustive_encoded_size(input));
        require_neutron_roundtrip(input, encoded);
    }
}

// Purpose: Cover full segment trees, short final segments and the largest admitted aggregate workspace.
// Inputs: Repeating RAM-only input at a segment boundary and the exact 1 MiB admission maximum.
// Outputs: Requires independent CPU/HIP recovery and a payload no larger than the ordinary level-nine parser.
TEST_CASE(neutron_dictionary_segment_and_workspace_boundaries) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron workspace qualification requires HIP\n";
        return;
    }
    for (const auto size : {std::size_t{kSegmentBytes} + 37U, kMaxNeutronBatchBytes}) {
        std::vector<std::byte> input(size);
        for (std::size_t position = 0U; position < size; ++position) {
            input[position] = static_cast<std::byte>((position % kSegmentBytes) % 63U);
        }
        const auto encoded = encode_neutron_segments(input);
        require_neutron_roundtrip(input, encoded);
        const auto ordinary = encode_segments(input, 9);
        std::size_t neutron_bytes = 0U;
        std::size_t ordinary_bytes = 0U;
        for (const auto& segment : encoded.segments) {
            neutron_bytes += segment.payload.size();
        }
        for (const auto& segment : ordinary.segments) {
            ordinary_bytes += segment.payload.size();
        }
        REQUIRE_TRUE(neutron_bytes <= ordinary_bytes);
        std::cout << "Neutron segment correctness input_bytes=" << size << " neutron_bytes=" << neutron_bytes
                  << " ordinary_level9_bytes=" << ordinary_bytes << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Prove bounded batching preserves every independent minimum-byte parse and reduces actual launch count.
// Inputs: Sixteen full segments mixing verified empty graphs and long matches, using at most one MiB per call.
// Outputs: Requires byte-identical segment payloads against four isolated calls and independent CPU/HIP readback.
TEST_CASE(neutron_dictionary_batched_parse_equals_isolated_blocks) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron batched parsing requires HIP\n";
        return;
    }
    const auto unique = unique_neutron_segment();
    std::vector<std::byte> input(kMaxNeutronBatchBytes);
    for (std::size_t offset = 0U; offset < input.size(); offset += kSegmentBytes) {
        const auto segment = offset / kSegmentBytes;
        if (segment % 3U == 0U) {
            std::copy(unique.begin(), unique.end(), input.begin() + static_cast<std::ptrdiff_t>(offset));
        } else {
            std::fill_n(input.begin() + static_cast<std::ptrdiff_t>(offset), kSegmentBytes,
                        static_cast<std::byte>(segment));
        }
    }
    const auto grouped = encode_neutron_segments(input);
    REQUIRE_EQ(grouped.segments.size(), input.size() / kSegmentBytes);
    std::uint64_t isolated_launches = 0U;
    constexpr auto block_bytes = kMaxNeutronBatchBytes / 4U;
    for (std::size_t offset = 0U; offset < input.size(); offset += block_bytes) {
        const auto isolated = encode_neutron_segments(std::span(input).subspan(offset, block_bytes));
        isolated_launches += isolated.explicit_kernel_launches;
        for (std::size_t index = 0U; index < isolated.segments.size(); ++index) {
            REQUIRE_EQ(grouped.segments[offset / kSegmentBytes + index].payload, isolated.segments[index].payload);
        }
    }
    REQUIRE_TRUE(grouped.explicit_kernel_launches < isolated_launches);
    require_neutron_roundtrip(input, grouped);
}

// Purpose: Preserve the exact ordinary first-stage winner when grouped dictionary trials lose but composition wins.
// Inputs: One MiB of authenticated unique-window segments and four independently framed native archive blocks.
// Outputs: Reconstructs every baseline byte/descriptor through the selected stage, with strict savings and dual
// readback.
TEST_CASE(neutron_dictionary_grouped_portfolio_preserves_winners) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "[SKIP] Neutron grouped portfolio requires HIP\n";
        return;
    }
    const auto unique = unique_neutron_segment();
    std::vector<std::byte> input(kMaxNeutronBatchBytes);
    for (std::size_t offset = 0U; offset < input.size(); offset += unique.size()) {
        std::copy(unique.begin(), unique.end(), input.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    constexpr auto block_bytes = static_cast<std::uint32_t>(kMaxNeutronBatchBytes / 4U);
    const auto baseline =
        superzip::encode_chunk(input, {.require_gpu = true, .block_size = block_bytes, .compression_level = 9});
    std::size_t checkpoints = 0U;
    const auto encoded =
        superzip::encode_chunk(input, {.require_gpu = true,
                                       .block_size = block_bytes,
                                       .compression_level = 9,
                                       .compression_mode = superzip::NativeCompressionMode::NeutronStar,
                                       .encode_checkpoint = [&] { ++checkpoints; }});
    REQUIRE_EQ(encoded.blocks.size(), 4U);
    std::vector<std::byte> restored_payload;
    std::size_t composed = 0U;
    for (std::size_t index = 0U; index < encoded.blocks.size(); ++index) {
        auto original = encoded.blocks[index];
        const auto payload =
            std::span(encoded.payload).subspan(static_cast<std::size_t>(original.encoded_offset), original.encoded_len);
        original.encoded_offset = restored_payload.size();
        if (original.kind == superzip::BlockKind::GpuBytePlane) {
            // A different permutation may now beat composition. Independently invert it before recreating the
            // unchanged ordinary GPU payload; retain the exact baseline byte and descriptor oracle below.
            std::vector<std::byte> restored_source(original.uncompressed_len);
            auto local = original;
            local.encoded_offset = 0U;
            superzip::decode_chunk_cpu(payload, std::span(&local, 1U), restored_source, {});
            REQUIRE_TRUE(
                std::ranges::equal(restored_source, std::span(input).subspan(index * block_bytes, block_bytes)));
            const auto restored = superzip::encode_chunk(
                restored_source, {.require_gpu = true, .block_size = block_bytes, .compression_level = 9});
            REQUIRE_EQ(restored.blocks.size(), 1U);
            REQUIRE_TRUE(payload.size() < restored.payload.size());
            original = restored.blocks.front();
            original.encoded_offset = restored_payload.size();
            restored_payload.insert(restored_payload.end(), restored.payload.begin(), restored.payload.end());
            ++composed;
        } else if (original.kind == superzip::BlockKind::GpuCompound) {
            const auto stages = superzip::parse_gpu_compound_block(payload, original);
            std::vector<std::byte> intermediate(stages.inner.uncompressed_len);
            superzip::decode_chunk_cpu(stages.inner_payload, std::span(&stages.inner, 1U), intermediate, {});
            original = stages.original;
            original.encoded_offset = restored_payload.size();
            REQUIRE_TRUE(payload.size() < intermediate.size());
            restored_payload.insert(restored_payload.end(), intermediate.begin(), intermediate.end());
            ++composed;
        } else {
            restored_payload.insert(restored_payload.end(), payload.begin(), payload.end());
        }
        REQUIRE_EQ(original.kind, baseline.blocks[index].kind);
        REQUIRE_EQ(original.encoded_len, baseline.blocks[index].encoded_len);
        REQUIRE_EQ(original.encoded_offset, baseline.blocks[index].encoded_offset);
    }
    REQUIRE_TRUE(composed > 0U);
    REQUIRE_EQ(restored_payload, baseline.payload);
    REQUIRE_TRUE(encoded.payload.size() < baseline.payload.size());
    REQUIRE_EQ(encoded.source_crc32, baseline.source_crc32);
    REQUIRE_TRUE(encoded.source_crc32_available && encoded.gpu_used);
    std::vector<std::byte> decoded(input.size());
    superzip::decode_chunk_cpu(encoded.payload, encoded.blocks, decoded, {});
    REQUIRE_EQ(decoded, input);
    REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, {.require_gpu = true}));
    REQUIRE_EQ(decoded, input);
    // The final observed checkpoint is the last secondary trial; cancellation must not enter a CPU fallback.
    std::size_t observed = 0U;
    bool cancelled = false;
    try {
        (void)superzip::encode_chunk(input, {.require_gpu = true,
                                             .block_size = block_bytes,
                                             .compression_level = 9,
                                             .compression_mode = superzip::NativeCompressionMode::NeutronStar,
                                             .encode_checkpoint = [&] {
                                                 if (++observed == checkpoints) {
                                                     throw superzip::ArchiveError("cancel secondary trial");
                                                 }
                                             }});
    } catch (const superzip::ArchiveError&) {
        cancelled = true;
    }
    REQUIRE_TRUE(cancelled);
    REQUIRE_EQ(observed, checkpoints);
    REQUIRE_EQ(
        superzip::encode_chunk(input, {.require_gpu = true, .block_size = block_bytes, .compression_level = 9}).payload,
        baseline.payload);
}
