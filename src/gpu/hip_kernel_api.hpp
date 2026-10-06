#pragma once

#include "gpu/hip_codec_support.hpp"
#include "gpu/hip_dictionary_optimal_state.hpp"
#include "gpu/sparse_pattern_device.hpp"

#include <type_traits>

namespace superzip::hip_detail {

inline constexpr std::uint32_t kDictionaryKernelThreads = 256U;
inline constexpr std::uint32_t kDictionaryMaxPeriodicMatchBytes = 32768U;
inline constexpr std::uint32_t kDictionaryPeriodicComparisonScale = 16U;
inline constexpr std::uint32_t kSparseKernelThreads = 256U;
inline constexpr std::uint32_t kSparseKernelTileBytes = 64U * 1024U;

struct PrefixDecodeSegment {
    std::uint64_t codebook_offset;
    std::uint64_t table_offset;
    std::uint64_t bitstream_offset;
    std::uint64_t output_offset;
    std::uint32_t segment_index;
    std::uint32_t decoded_len;
    std::uint32_t adaptive;
};

struct CrcInputRange {
    std::uint64_t offset;
    std::uint32_t length;
};

struct PrefixEncodeSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
};

struct AdaptiveEncodeTable {
    std::uint16_t code[256];
    std::uint8_t width[256];
};

struct AdaptiveEncodeSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
    std::uint32_t table_index;
};

struct EntropyLengthSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
    std::uint32_t table_index;
    std::uint32_t candidate_count;
    std::uint32_t length_offset;
};

}  // namespace superzip::hip_detail

namespace superzip {

// Purpose: Describe the private versioned kernel ABI without exporting heap owners or C++ exceptions.
// Inputs: Every pointer refers to one registered kernel or a bounded rocPRIM operation in the admitted module.
// Outputs: Immutable process-lifetime dispatch metadata; host code retains all allocation and error ownership.
struct HipKernelApi {
    std::uint32_t abi_version = 1U;
    std::uint32_t abi_bytes = sizeof(HipKernelApi);
    void (*materialize_prefix_segments_kernel)(const std::byte*, const hip_detail::PrefixDecodeSegment*, std::uint32_t,
                                               std::byte*, std::uint32_t*) = nullptr;
    void (*verify_analysis_candidates_kernel)(const std::byte*, std::size_t, const hip_detail::DeviceBlock*,
                                              std::uint32_t*, std::uint32_t, std::uint32_t) = nullptr;
    void (*materialize_blocks_kernel)(const std::byte*, const hip_detail::DeviceBlock*, std::uint32_t, std::byte*,
                                      std::size_t) = nullptr;
    void (*materialize_segments_kernel)(const std::byte*, const hip_detail::DeviceBlock*, std::uint32_t, std::byte*,
                                        std::size_t) = nullptr;
    void (*apply_sparse_patches_kernel)(const std::byte*, const hip_detail::DeviceBlock*, std::byte*,
                                        std::uint32_t) = nullptr;
    void (*crc32_segments_kernel)(const std::byte*, std::size_t, hip_detail::DeviceCrcSegment*, std::uint32_t,
                                  std::uint32_t) = nullptr;
    void (*crc32_independent_ranges_kernel)(const std::byte*, const hip_detail::CrcInputRange*,
                                            hip_detail::DeviceCrcSegment*, std::uint32_t) = nullptr;
    void (*decoded_crc32_segments_kernel)(const std::byte*, const hip_detail::DeviceBlock*, std::uint32_t, std::size_t,
                                          hip_detail::DeviceCrcSegment*, std::uint32_t, std::uint32_t) = nullptr;
    void (*diagnostic_compute_kernel)(std::uint32_t*, std::size_t, std::uint32_t, std::uint32_t) = nullptr;
    void (*diagnostic_checksum_kernel)(const std::uint32_t*, std::size_t, unsigned long long*) = nullptr;
    void (*prefix_segment_lengths_batch_kernel)(const std::byte*, const hip_detail::PrefixEncodeSegmentPlan*,
                                                std::uint32_t*, std::uint32_t) = nullptr;
    void (*prefix_pack_segments_batch_kernel)(const std::byte*, const hip_detail::PrefixEncodeSegmentPlan*,
                                              const std::uint32_t*, std::byte*, std::uint32_t) = nullptr;
    void (*entropy_lengths_2)(const std::byte*, const hip_detail::EntropyLengthSegmentPlan*,
                              const hip_detail::AdaptiveEncodeTable*, std::uint32_t*, std::uint32_t) = nullptr;
    void (*entropy_lengths_4)(const std::byte*, const hip_detail::EntropyLengthSegmentPlan*,
                              const hip_detail::AdaptiveEncodeTable*, std::uint32_t*, std::uint32_t) = nullptr;
    void (*entropy_lengths_8)(const std::byte*, const hip_detail::EntropyLengthSegmentPlan*,
                              const hip_detail::AdaptiveEncodeTable*, std::uint32_t*, std::uint32_t) = nullptr;
    void (*entropy_lengths_16)(const std::byte*, const hip_detail::EntropyLengthSegmentPlan*,
                               const hip_detail::AdaptiveEncodeTable*, std::uint32_t*, std::uint32_t) = nullptr;
    void (*adaptive_prefix_pack_segments_batch_kernel)(const std::byte*, const hip_detail::AdaptiveEncodeSegmentPlan*,
                                                       const hip_detail::AdaptiveEncodeTable*, const std::uint32_t*,
                                                       std::byte*, std::uint32_t) = nullptr;
    void (*decode_dictionary_segments)(const std::byte*, const DictionarySegmentSpan*, std::byte*,
                                       std::uint32_t*) = nullptr;
    void (*build_dictionary_keys)(const std::byte*, std::uint32_t, std::uint64_t*) = nullptr;
    void (*link_dictionary_predecessors)(const std::uint64_t*, std::uint32_t, std::uint32_t*) = nullptr;
    void (*search_dictionary_matches)(const std::byte*, std::uint32_t, const std::uint32_t*, dictionary::Effort,
                                      dictionary::Match*) = nullptr;
    void (*search_neutron_matches)(const std::byte*, std::uint32_t, const std::uint32_t*, std::uint32_t, std::uint32_t,
                                   dictionary::Match*) = nullptr;
    void (*encode_dictionary_deferred)(const std::byte*, std::uint32_t, const std::uint32_t*, const std::uint16_t*,
                                       dictionary::Effort, std::byte*, std::uint32_t*) = nullptr;
    void (*encode_dictionary_cached)(const std::byte*, std::uint32_t, const std::uint32_t*, const std::uint16_t*,
                                     dictionary::Effort, std::byte*, std::uint32_t*) = nullptr;
    void (*encode_dictionary_periodic)(const std::byte*, std::uint32_t, const std::uint32_t*, const std::uint16_t*,
                                       dictionary::Effort, std::byte*, std::uint32_t*) = nullptr;
    void (*compact_dictionary_segments)(const std::byte*, const std::uint32_t*, std::uint32_t, std::byte*) = nullptr;
    void (*count_sparse_positions_kernel)(const std::byte*, const sparse_pattern::SparseCandidate*,
                                          std::uint32_t*) = nullptr;
    void (*gather_sparse_positions_kernel)(const std::byte*, const sparse_pattern::SparseCandidate*, std::uint32_t*,
                                           std::uint32_t*) = nullptr;
    void (*neutron_bound_match_graphs)(dictionary::optimal::State, std::uint32_t,
                                       dictionary::optimal::EmitCursor*) = nullptr;
    void (*neutron_initialize)(dictionary::optimal::State) = nullptr;
    void (*neutron_classify_match_graphs)(dictionary::optimal::State, std::uint32_t, std::uint32_t*) = nullptr;
    void (*neutron_initialize_noncompetitive_graphs)(dictionary::optimal::State, std::uint32_t, std::uint32_t,
                                                     std::uint32_t*) = nullptr;
    void (*neutron_parse_tile)(dictionary::optimal::State, std::uint32_t, std::uint32_t,
                               const std::uint32_t*) = nullptr;
    void (*neutron_initialize_writers)(dictionary::optimal::EmitCursor*, std::uint32_t*, std::uint32_t) = nullptr;
    void (*neutron_emit_tile)(const std::byte*, std::uint32_t, dictionary::optimal::State, std::byte*,
                              dictionary::optimal::EmitCursor*, std::uint32_t*) = nullptr;
    void (*byte_plane_transform)(const std::byte*, std::byte*, std::uint32_t, std::uint32_t, bool) = nullptr;
    hipError_t (*sort_dictionary_keys)(void*, std::size_t*, const std::uint64_t*, std::uint64_t*, std::uint32_t,
                                       hipStream_t) noexcept = nullptr;
};

static_assert(std::is_standard_layout_v<HipKernelApi> && std::is_trivially_copyable_v<HipKernelApi>);
static_assert(sizeof(HipKernelApi) == 304U && alignof(HipKernelApi) == alignof(void*));
static_assert(std::is_trivially_copyable_v<hip_detail::PrefixDecodeSegment> &&
              sizeof(hip_detail::PrefixDecodeSegment) == 48U);
static_assert(std::is_trivially_copyable_v<hip_detail::CrcInputRange> && sizeof(hip_detail::CrcInputRange) == 16U);
static_assert(std::is_trivially_copyable_v<hip_detail::PrefixEncodeSegmentPlan> &&
              sizeof(hip_detail::PrefixEncodeSegmentPlan) == 16U);
static_assert(std::is_trivially_copyable_v<hip_detail::AdaptiveEncodeTable> &&
              sizeof(hip_detail::AdaptiveEncodeTable) == 768U);
static_assert(std::is_trivially_copyable_v<hip_detail::AdaptiveEncodeSegmentPlan> &&
              sizeof(hip_detail::AdaptiveEncodeSegmentPlan) == 24U);
static_assert(std::is_trivially_copyable_v<hip_detail::EntropyLengthSegmentPlan> &&
              sizeof(hip_detail::EntropyLengthSegmentPlan) == 32U);

// Purpose: Admit the exact kernel module only after trusted HIP runtime/device readiness.
// Inputs: None; the embedded module digest and private ABI identify this build's kernel payload.
// Outputs: Returns validated dispatch metadata, or throws before an unverified kernel can execute.
const HipKernelApi& hip_kernel_api();

}  // namespace superzip
