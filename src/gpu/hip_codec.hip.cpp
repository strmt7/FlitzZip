#include "gpu/gpu_codec.hpp"
#include "gpu/hip_device.hpp"
#include "gpu/hip_codec_support.hpp"
#include "gpu/crc32_device.hpp"
#include "gpu/dictionary_device.hpp"
#include "gpu/sparse_pattern_candidate.hpp"
#include "gpu/dictionary_candidate.hpp"
#include "gpu/byte_plane_transform.hip.hpp"

#include "core/checksum.hpp"
#include "core/host_memory_budget.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <hip/hip_runtime.h>

namespace superzip {

namespace {

using namespace hip_detail;

__device__ __constant__ std::uint32_t kDeviceCrc32Table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU, 0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U, 0x0EDB8832U,
    0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U, 0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U, 0x1DB71064U, 0x6AB020F2U,
    0xF3B97148U, 0x84BE41DEU, 0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U, 0x136C9856U, 0x646BA8C0U, 0xFD62F97AU,
    0x8A65C9ECU, 0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U, 0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU, 0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U, 0x32D86CE3U,
    0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U, 0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U, 0x21B4F4B5U, 0x56B3C423U,
    0xCFBA9599U, 0xB8BDA50FU, 0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U, 0x2F6F7C87U, 0x58684C11U, 0xC1611DABU,
    0xB6662D3DU, 0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU, 0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U, 0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U, 0x6B6B51F4U,
    0x1C6C6162U, 0x856530D8U, 0xF262004EU, 0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U, 0x65B0D9C6U, 0x12B7E950U,
    0x8BBEB8EAU, 0xFCB9887CU, 0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U, 0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U,
    0xD4BB30E2U, 0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU, 0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U, 0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U, 0x5768B525U,
    0x206F85B3U, 0xB966D409U, 0xCE61E49FU, 0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U, 0x59B33D17U, 0x2EB40D81U,
    0xB7BD5C3BU, 0xC0BA6CADU, 0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU, 0xEAD54739U, 0x9DD277AFU, 0x04DB2615U,
    0x73DC1683U, 0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U, 0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU, 0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U, 0xFED41B76U,
    0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU, 0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U, 0xD6D6A3E8U, 0xA1D1937EU,
    0x38D8C2C4U, 0x4FDFF252U, 0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU, 0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U,
    0x41047A60U, 0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U, 0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU, 0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U, 0xC2D7FFA7U,
    0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU, 0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU, 0x9C0906A9U, 0xEB0E363FU,
    0x72076785U, 0x05005713U, 0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U, 0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U,
    0x0BDBDF21U, 0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU, 0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU, 0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U, 0xA00AE278U,
    0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U, 0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU, 0xAED16A4AU, 0xD9D65ADCU,
    0x40DF0B66U, 0x37D83BF0U, 0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U, 0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U,
    0x24B4A3A6U, 0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU, 0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU,
};

// Purpose: Declare the tiled HIP candidate verifier before host helper dispatch.
// Inputs: See the definition below.
// Outputs: Writes per-block mismatch flags into device memory.
__global__ void verify_analysis_candidates_kernel(const std::byte* input, std::size_t input_len,
                                                  const DeviceBlock* candidates, std::uint32_t* mismatches,
                                                  std::uint32_t block_count, std::uint32_t segments_per_block);

struct PrefixDecodeSegment {
    std::uint64_t codebook_offset;
    std::uint64_t table_offset;
    std::uint64_t bitstream_offset;
    std::uint64_t output_offset;
    std::uint32_t segment_index;
    std::uint32_t decoded_len;
    std::uint32_t adaptive;
};

// Purpose: Read one little-endian 32-bit value from device payload memory.
// Inputs: `bytes` points at at least four bytes.
// Outputs: Returns the decoded offset value.
__device__ std::uint32_t gpu_prefix_read_u32(const std::byte* bytes) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[3])) << 24U);
}

// Purpose: Peek a complete GPU prefix codeword with bounded byte loads.
// Inputs: `stream` is a byte-aligned segment; `bit_pos` and `limit_bits` bound readable bits.
// Outputs: Returns up to twelve low-order code bits, zero-padding a truncated segment.
__device__ std::uint32_t gpu_prefix_peek_code(const std::byte* stream, std::uint32_t bit_pos,
                                              std::uint32_t limit_bits) {
    if (bit_pos >= limit_bits) {
        return 0U;
    }
    const auto byte_index = bit_pos >> 3U;
    const auto bit_shift = bit_pos & 7U;
    const auto byte_count = limit_bits >> 3U;
    std::uint32_t code = static_cast<std::uint8_t>(stream[byte_index]);
    if (byte_index + 1U < byte_count) {
        code |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(stream[byte_index + 1U])) << 8U;
    }
    if (bit_shift > 4U && byte_index + 2U < byte_count) {
        code |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(stream[byte_index + 2U])) << 16U;
    }
    return (code >> bit_shift) & 0xFFFU;
}

// Purpose: Advance a prefix cursor only when a complete codeword remains.
// Inputs: `bit_pos` is within `limit_bits`, and `width` is the decoded codeword width.
// Outputs: Advances `bit_pos` and returns true, or leaves it unchanged and returns false for truncation.
__device__ bool gpu_prefix_advance(std::uint32_t& bit_pos, std::uint32_t limit_bits, std::uint32_t width) {
    if (width > limit_bits - bit_pos) {
        return false;
    }
    bit_pos += width;
    return true;
}

// Purpose: Decode one byte from the static GPU prefix code.
// Inputs: `stream`, `bit_pos`, and `limit_bits` describe one encoded segment; `valid` receives grammar status.
// Outputs: Returns a decoded byte or clears `valid` for truncation or an invalid high-byte payload.
__device__ std::byte gpu_prefix_decode_byte(const std::byte* stream, std::uint32_t& bit_pos, std::uint32_t limit_bits,
                                            bool& valid) {
    const auto code = gpu_prefix_peek_code(stream, bit_pos, limit_bits);
    if ((code & 1U) == 0U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 3U);
        return static_cast<std::byte>((code >> 1U) & 3U);
    }
    if ((code & 3U) == 1U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 6U);
        return static_cast<std::byte>(4U + ((code >> 2U) & 15U));
    }
    if ((code & 7U) == 3U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 9U);
        return static_cast<std::byte>(20U + ((code >> 3U) & 63U));
    }
    valid = gpu_prefix_advance(bit_pos, limit_bits, 11U);
    const auto high = (code >> 3U) & 255U;
    valid = valid && high <= 171U;
    return static_cast<std::byte>(84U + high);
}

// Purpose: Decode one byte from an adaptive GPU prefix segment.
// Inputs: `codebook`, `stream`, `bit_pos`, and `limit_bits` describe the encoded byte stream; `valid` receives status.
// Outputs: Returns a decoded byte and clears `valid` when the codeword is incomplete.
__device__ std::byte gpu_adaptive_prefix_decode_byte(const std::byte* codebook, const std::byte* stream,
                                                     std::uint32_t& bit_pos, std::uint32_t limit_bits, bool& valid) {
    const auto code = gpu_prefix_peek_code(stream, bit_pos, limit_bits);
    if ((code & 1U) == 0U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 3U);
        return codebook[(code >> 1U) & 3U];
    }
    if ((code & 3U) == 1U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 6U);
        return codebook[kGpuAdaptivePrefixSmallSymbols + ((code >> 2U) & 15U)];
    }
    if ((code & 7U) == 3U) {
        valid = gpu_prefix_advance(bit_pos, limit_bits, 9U);
        return codebook[kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols + ((code >> 3U) & 63U)];
    }
    valid = gpu_prefix_advance(bit_pos, limit_bits, 11U);
    return static_cast<std::byte>((code >> 3U) & 255U);
}

// Purpose: Decode one byte through a bounded 12-bit version-eight Huffman lookup.
// Inputs: `lookup`, `stream`, and bit bounds identify one encoded codeword; `valid` receives grammar status.
// Outputs: Returns the symbol and advances the cursor only for a complete valid codeword.
__device__ std::byte gpu_huffman_decode_byte(const std::byte* lookup, const std::byte* stream, std::uint32_t& bit_pos,
                                             std::uint32_t limit_bits, bool& valid) {
    const auto code = gpu_prefix_peek_code(stream, bit_pos, limit_bits);
    const auto entry = static_cast<std::uint32_t>(static_cast<std::uint8_t>(lookup[code * 2U])) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(lookup[code * 2U + 1U])) << 8U);
    const auto width = entry >> 8U;
    valid = width != 0U && width <= kGpuHuffmanLookupBits && gpu_prefix_advance(bit_pos, limit_bits, width);
    return static_cast<std::byte>(entry & 0xFFU);
}

// Purpose: Decode GPU prefix segments into the final device output buffer.
// Inputs: `payload` is the encoded archive payload, `plans` describes each prefix segment, `output` is decoded
// storage, and `errors` receives one grammar result per plan. Outputs: Writes valid bytes and flags malformed plans.
__global__ void materialize_prefix_segments_kernel(const std::byte* payload, const PrefixDecodeSegment* plans,
                                                   std::uint32_t plan_count, std::byte* output, std::uint32_t* errors) {
    const auto plan_index = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (plan_index >= plan_count) {
        return;
    }
    const auto& plan = plans[plan_index];
    const auto* table = payload + plan.table_offset;
    const auto encoded_start = gpu_prefix_read_u32(table + (static_cast<std::size_t>(plan.segment_index) * 4U));
    const auto encoded_end = gpu_prefix_read_u32(table + ((static_cast<std::size_t>(plan.segment_index) + 1U) * 4U));
    if (encoded_end < encoded_start) {
        errors[plan_index] = 1U;
        return;
    }
    const auto* stream = payload + plan.bitstream_offset + encoded_start;
    const auto limit_bits = (encoded_end - encoded_start) * 8U;
    const auto* codebook = payload + plan.codebook_offset;
    std::uint32_t bit_pos = 0;
    for (std::uint32_t i = 0; i < plan.decoded_len; ++i) {
        bool valid = true;
        const auto decoded = plan.adaptive == 2U ? gpu_huffman_decode_byte(codebook, stream, bit_pos, limit_bits, valid)
                             : plan.adaptive == 1U
                                 ? gpu_adaptive_prefix_decode_byte(codebook, stream, bit_pos, limit_bits, valid)
                                 : gpu_prefix_decode_byte(stream, bit_pos, limit_bits, valid);
        if (!valid) {
            errors[plan_index] = 1U;
            return;
        }
        output[plan.output_offset + i] = decoded;
    }
    errors[plan_index] = 0U;
}

// Purpose: Resolve a provisional candidate kind after GPU verification.
// Inputs: `candidate` is the host candidate and `mismatch` is the GPU-produced rejection flag.
// Outputs: Returns the verified block kind, or Raw when the candidate failed full verification.
std::uint8_t verified_candidate_kind(const DeviceBlock& candidate, std::uint32_t mismatch) {
    return mismatch == 0U ? candidate.kind : static_cast<std::uint8_t>(BlockKind::Raw);
}

// Purpose: Verify sampled fill/pattern candidates on the GPU with tiled parallel work.
// Inputs: `device_input` is the chunk in VRAM, `input_len`, `block_size`, and `candidates` describe encode work, and
// `telemetry` records HIP transfers, allocations, and kernel time.
// Outputs: Returns one mismatch flag per archive block; all zeros when no candidate verification is needed.
std::vector<std::uint32_t> verify_encode_analysis_candidates_device(const std::byte* device_input,
                                                                    std::size_t input_len, std::uint32_t block_size,
                                                                    std::span<const DeviceBlock> candidates,
                                                                    GpuTelemetry* telemetry) {
    std::vector<std::uint32_t> mismatches(candidates.size(), 0U);
    if (!has_non_raw_analysis_candidate(candidates)) {
        return mismatches;
    }
    const auto block_count = static_cast<std::uint32_t>(candidates.size());
    const auto candidate_table_bytes = checked_multiply_bytes(block_count, sizeof(DeviceBlock), "encode candidates");
    const auto mismatch_table_bytes = checked_multiply_bytes(block_count, sizeof(std::uint32_t), "encode mismatches");
    const auto required_bytes =
        checked_add_bytes(candidate_table_bytes, mismatch_table_bytes, "encode candidate verification memory");
    HipDeviceMemoryReservation reservation(required_bytes, "encode candidate verification");
    HipDeviceBuffer<DeviceBlock> device_candidates(candidate_table_bytes, "hipMalloc encode candidates");
    HipDeviceBuffer<std::uint32_t> device_mismatches(mismatch_table_bytes, "hipMalloc encode mismatches");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(
        copy_on_codec_stream(device_candidates.get(), candidates.data(), candidate_table_bytes, hipMemcpyHostToDevice),
        "hipMemcpy encode candidates");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_table_bytes));
    check_hip(hipMemsetAsync(device_mismatches.get(), 0, mismatch_table_bytes, hipStreamPerThread),
              "hipMemset encode mismatches");
    const auto segments_per_block = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(block_size) + kAnalyzeSegmentBytes - 1U) / kAnalyzeSegmentBytes);
    const auto grid64 = static_cast<std::uint64_t>(segments_per_block) * block_count;
    if (grid64 > std::numeric_limits<unsigned int>::max()) {
        throw GpuError("encode candidate segment count exceeds HIP launch limits");
    }
    auto events = make_hip_event_pair("create verify_analysis_candidates_kernel events");
    launch_measured_kernel(verify_analysis_candidates_kernel, static_cast<unsigned int>(grid64), 256, 0,
                           hipStreamPerThread, events, "launch verify_analysis_candidates_kernel", device_input,
                           input_len, device_candidates.get(), device_mismatches.get(), block_count,
                           segments_per_block);
    finish_measured_kernel(telemetry, events, "synchronize verify_analysis_candidates_kernel");
    check_hip(
        copy_on_codec_stream(mismatches.data(), device_mismatches.get(), mismatch_table_bytes, hipMemcpyDeviceToHost),
        "hipMemcpy encode mismatches");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(mismatch_table_bytes));
    device_candidates.reset_checked("hipFree encode candidates");
    device_mismatches.reset_checked("hipFree encode mismatches");
    return mismatches;
}

// Purpose: Choose the smallest GPU-native replacement for verified raw blocks allowed by the compression level.
// Inputs: `device_input`, `input`, block settings, `source_blocks`, `compression_level`, and `telemetry` describe one
// uploaded chunk.
// Outputs: Returns the best GPU-native encoded chunk, or empty when raw storage is smallest.
std::optional<EncodedChunk> encode_native_prefix_chunk_device(const std::byte* device_input,
                                                              std::span<const std::byte> input,
                                                              std::uint32_t block_size,
                                                              std::span<const BlockDescriptor> source_blocks,
                                                              int compression_level, GpuTelemetry* telemetry) {
    auto fixed = encode_prefix_chunk_device(device_input, input, block_size, source_blocks, telemetry);
    if (compression_level == 1) {
        return fixed;
    }
    auto entropy = encode_entropy_prefix_chunk_device(device_input, input, block_size, source_blocks,
                                                      fixed ? &*fixed : nullptr, compression_level, telemetry);
    return entropy ? std::move(entropy) : std::move(fixed);
}

// Purpose: Count only GPU block kinds present after all competing native encoders have been compared.
// Inputs: A fully selected encoded chunk and operation-owned telemetry.
// Outputs: Adds emitted prefix, dictionary, and sparse counts without counting discarded trials.
void record_selected_block_kinds(const EncodedChunk& chunk, GpuTelemetry* telemetry) {
    std::uint64_t prefixes = 0U;
    std::uint64_t dictionaries = 0U;
    std::uint64_t sparse_blocks = 0U;
    for (const auto& block : chunk.blocks) {
        auto original = block;
        if (block.kind == BlockKind::GpuBytePlaneContexts) {
            const auto stages = parse_gpu_byte_plane_contexts(
                std::span(chunk.payload).subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len),
                block);
            for (const auto& inner : std::span(stages.blocks).first(stages.width)) {
                prefixes += is_gpu_prefix_block(inner);
                dictionaries += inner.kind == BlockKind::GpuDictionary;
                sparse_blocks += is_gpu_sparse_pattern_kind(inner.kind);
            }
            continue;
        }
        if (block.kind == BlockKind::GpuBytePlane) {
            original =
                parse_gpu_byte_plane_block(
                    std::span(chunk.payload).subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len),
                    block)
                    .inner;
        }
        if (block.kind == BlockKind::GpuCompound) {
            original =
                parse_gpu_compound_block(
                    std::span(chunk.payload).subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len),
                    block)
                    .original;
        }
        prefixes += is_gpu_prefix_block(original);
        dictionaries += original.kind == BlockKind::GpuDictionary;
        sparse_blocks += is_gpu_sparse_pattern_kind(original.kind);
    }
    record_gpu_prefix_blocks(telemetry, prefixes);
    record_gpu_dictionary_blocks(telemetry, dictionaries);
    record_gpu_sparse_pattern_blocks(telemetry, sparse_blocks);
}

// Purpose: Convert verified encode candidates into SUZIP block descriptors.
// Inputs: `candidates` and `mismatches` describe one encoded chunk after GPU verification.
// Outputs: Populates `out.blocks`, returns true when every block is raw, and records the encoded payload size.
bool append_verified_encode_descriptors(EncodedChunk& out, std::span<const DeviceBlock> candidates,
                                        std::span<const std::uint32_t> mismatches, std::uint64_t& encoded_offset,
                                        std::uint64_t& pattern_blocks) {
    bool all_raw = true;
    out.blocks.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        const auto len = static_cast<std::size_t>(candidate.uncompressed_len);
        const auto effective_kind = verified_candidate_kind(candidate, mismatches[i]);
        if (effective_kind == static_cast<std::uint8_t>(BlockKind::Fill)) {
            all_raw = false;
            out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Fill,
                                                 .fill_value = candidate.fill_value,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = encoded_offset,
                                                 .encoded_len = 0});
        } else if (effective_kind == static_cast<std::uint8_t>(BlockKind::Pattern)) {
            all_raw = false;
            ++pattern_blocks;
            out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Pattern,
                                                 .fill_value = 0,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = encoded_offset,
                                                 .encoded_len = candidate.encoded_len});
            encoded_offset += candidate.encoded_len;
        } else {
            out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Raw,
                                                 .fill_value = 0,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = encoded_offset,
                                                 .encoded_len = static_cast<std::uint32_t>(len)});
            encoded_offset += len;
        }
    }
    return all_raw;
}

// Purpose: Publish verified baseline payload bytes while retaining the owned all-raw move fast path.
// Inputs: `input` and optional `owned_input` share storage; block settings and verification data describe output.
// Outputs: Moves owned all-raw bytes when available, or appends raw/compact pattern bytes in descriptor order.
void append_verified_encode_payload(EncodedChunk& out, std::span<const std::byte> input, std::uint32_t block_size,
                                    std::span<const DeviceBlock> candidates, std::span<const std::uint32_t> mismatches,
                                    bool all_raw, std::vector<std::byte>* owned_input) {
    if (all_raw) {
        if (owned_input != nullptr) {
            out.payload = std::move(*owned_input);
        } else {
            out.payload.assign(input.begin(), input.end());
        }
        return;
    }
    out.payload.reserve(input.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto effective_kind = verified_candidate_kind(candidates[i], mismatches[i]);
        if (effective_kind == static_cast<std::uint8_t>(BlockKind::Fill)) {
            continue;
        }
        const auto start = static_cast<std::size_t>(candidates[i].output_offset);
        const auto len = static_cast<std::size_t>(candidates[i].uncompressed_len);
        if (start > input.size() || len > input.size() - start || len > block_size) {
            throw GpuError("verified encode block exceeds its input layout");
        }
        const auto encoded_len = effective_kind == static_cast<std::uint8_t>(BlockKind::Pattern)
                                     ? static_cast<std::size_t>(candidates[i].encoded_len)
                                     : len;
        out.payload.insert(out.payload.end(), input.begin() + start, input.begin() + start + encoded_len);
    }
}

// Purpose: Verify provisional fill/pattern encode candidates over fixed-size GPU tiles.
// Inputs: `input`, `input_len`, `candidates`, and `segments_per_block` describe candidate work; `mismatches` is one
// flag per block. Outputs: Atomically marks blocks whose sampled candidate does not describe the full block.
__global__ void verify_analysis_candidates_kernel(const std::byte* input, std::size_t input_len,
                                                  const DeviceBlock* candidates, std::uint32_t* mismatches,
                                                  std::uint32_t block_count, std::uint32_t segments_per_block) {
    const auto global_segment = static_cast<std::uint32_t>(blockIdx.x);
    const auto block_index = global_segment / segments_per_block;
    if (block_index >= block_count) {
        return;
    }
    const auto& candidate = candidates[block_index];
    if (candidate.kind == static_cast<std::uint8_t>(BlockKind::Raw)) {
        return;
    }
    const auto segment_index = global_segment % segments_per_block;
    const auto block_start = static_cast<std::size_t>(candidate.output_offset);
    const auto block_end = min(block_start + static_cast<std::size_t>(candidate.uncompressed_len), input_len);
    const auto segment_start = block_start + static_cast<std::size_t>(segment_index) * kAnalyzeSegmentBytes;
    if (segment_start >= block_end) {
        return;
    }
    const auto segment_end = min(segment_start + static_cast<std::size_t>(kAnalyzeSegmentBytes), block_end);
    bool mismatch = false;
    if (candidate.kind == static_cast<std::uint8_t>(BlockKind::Fill)) {
        for (auto pos = segment_start + threadIdx.x; pos < segment_end; pos += blockDim.x) {
            if ((static_cast<unsigned int>(input[pos]) & 0xFFU) != candidate.fill_value) {
                mismatch = true;
                break;
            }
        }
    } else if (candidate.kind == static_cast<std::uint8_t>(BlockKind::Pattern) && candidate.encoded_len >= 2U &&
               candidate.encoded_len <= kMaxGpuPatternBytes && candidate.encoded_len < candidate.uncompressed_len) {
        const auto period = static_cast<std::size_t>(candidate.encoded_len);
        for (auto pos = segment_start + threadIdx.x; pos < segment_end; pos += blockDim.x) {
            const auto expected = input[block_start + ((pos - block_start) % period)];
            if (input[pos] != expected) {
                mismatch = true;
                break;
            }
        }
    } else {
        mismatch = true;
    }
    if (mismatch) {
        atomicExch(&mismatches[block_index], 1U);
    }
}

// Purpose: Locate the decoded block that contains one output byte offset.
// Inputs: `blocks`/`block_count` describe a validated decoded layout and `output_offset` is a byte position.
// Outputs: Returns a block index less than `block_count`, or `block_count` if metadata is inconsistent.
__device__ std::uint32_t find_decoded_block(const DeviceBlock* blocks, std::uint32_t block_count,
                                            std::size_t output_offset);

// Purpose: Read a validated little-endian sparse field from device payload bytes.
// Inputs: `bytes` points to four resident bytes admitted by the host parser.
// Outputs: Returns the unsigned field without relying on alignment.
__device__ std::uint32_t read_sparse_u32_device(const std::byte* bytes) {
    std::uint32_t value = 0U;
    for (std::uint32_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

// Purpose: Decode fill/raw block metadata into output bytes on the AMD GPU.
// Inputs: `payload`, `blocks`, `block_count`, `output`, and `output_len` are device pointers/counts validated by the
// host path. Outputs: Writes decoded bytes into `output`.
__global__ void materialize_blocks_kernel(const std::byte* payload, const DeviceBlock* blocks,
                                          std::uint32_t block_count, std::byte* output, std::size_t output_len) {
    const auto block_index = static_cast<std::uint32_t>(blockIdx.x);
    if (block_index >= block_count) {
        return;
    }
    const auto& block = blocks[block_index];
    if (block.output_offset > output_len || block.uncompressed_len > output_len - block.output_offset) {
        return;
    }
    for (std::size_t in_block = threadIdx.x; in_block < block.uncompressed_len; in_block += blockDim.x) {
        const auto output_index = block.output_offset + in_block;
        if (block.kind == 1) {
            output[output_index] = static_cast<std::byte>(block.fill_value);
        } else if (block.kind == 0) {
            output[output_index] = payload[block.encoded_offset + in_block];
        } else if (block.kind == 3) {
            output[output_index] = payload[block.encoded_offset + (in_block % block.encoded_len)];
        }
    }
}

// Purpose: Decode fill/raw/pattern metadata over fixed output segments to improve occupancy for large SUZIP blocks.
// Inputs: `payload`, `blocks`, `block_count`, `output`, and `output_len` are validated device buffers and bounds.
// Outputs: Writes raw/fill/pattern bytes, skips separate decoder windows, and preserves each lane's byte stride.
__global__ void materialize_segments_kernel(const std::byte* payload, const DeviceBlock* blocks,
                                            std::uint32_t block_count, std::byte* output, std::size_t output_len) {
    const auto segment_start = static_cast<std::size_t>(blockIdx.x) * kMaterializeSegmentBytes;
    if (segment_start >= output_len) {
        return;
    }
    const auto segment_end = min(segment_start + static_cast<std::size_t>(kMaterializeSegmentBytes), output_len);
    auto pos = segment_start + static_cast<std::size_t>(threadIdx.x);
    auto block_index = find_decoded_block(blocks, block_count, pos);
    while (pos < segment_end && block_index < block_count) {
        const auto& block = blocks[block_index];
        const auto block_start = static_cast<std::size_t>(block.output_offset);
        const auto block_end = block_start + static_cast<std::size_t>(block.uncompressed_len);
        if (block.kind == static_cast<std::uint8_t>(BlockKind::Pattern) ||
            block.kind == static_cast<std::uint8_t>(BlockKind::GpuSparsePattern) ||
            block.kind == static_cast<std::uint8_t>(BlockKind::GpuLongSparsePattern)) {
            const bool sparse = block.kind != static_cast<std::uint8_t>(BlockKind::Pattern);
            const auto period = sparse ? read_sparse_u32_device(payload + block.encoded_offset) : block.encoded_len;
            const auto motif_offset = block.encoded_offset + (sparse ? kSparsePatternHeaderBytes : 0U);
            const auto stride = static_cast<std::uint32_t>(blockDim.x) % period;
            auto phase = static_cast<std::uint32_t>((pos - block_start) % period);
            while (pos < block_end && pos < segment_end) {
                output[pos] = payload[motif_offset + phase];
                phase += stride;
                if (phase >= period) {
                    phase -= period;
                }
                pos += blockDim.x;
            }
        } else if (block.kind != static_cast<std::uint8_t>(BlockKind::Raw) &&
                   block.kind != static_cast<std::uint8_t>(BlockKind::Fill)) {
            // A lane can jump over several short blocks; never subtract an already passed boundary.
            if (pos < block_end) {
                const auto remaining = min(block_end, segment_end) - pos;
                pos += ((remaining + blockDim.x - 1U) / blockDim.x) * blockDim.x;
            }
        } else {
            while (pos < block_end && pos < segment_end) {
                if (block.kind == 1) {
                    output[pos] = static_cast<std::byte>(block.fill_value);
                } else if (block.kind == 0) {
                    output[pos] = payload[block.encoded_offset + (pos - block_start)];
                }
                pos += blockDim.x;
            }
        }
        ++block_index;
    }
}

// Purpose: Apply admitted sorted corrections after the segmented motif expansion has completed.
// Inputs: `payload`, `blocks`, and `output` are device buffers whose sparse tables passed host validation.
// Outputs: Writes only disjoint patch positions inside each sparse block's decoded window.
__global__ void apply_sparse_patches_kernel(const std::byte* payload, const DeviceBlock* blocks, std::byte* output,
                                            std::uint32_t block_count) {
    const auto block_index = static_cast<std::uint32_t>(blockIdx.x);
    if (block_index >= block_count) {
        return;
    }
    const auto& block = blocks[block_index];
    if (block.kind != static_cast<std::uint8_t>(BlockKind::GpuSparsePattern) &&
        block.kind != static_cast<std::uint8_t>(BlockKind::GpuLongSparsePattern)) {
        return;
    }
    const auto* encoded = payload + block.encoded_offset;
    const auto period = read_sparse_u32_device(encoded);
    const auto patch_count = read_sparse_u32_device(encoded + sizeof(std::uint32_t));
    const auto* patches = encoded + kSparsePatternHeaderBytes + period;
    for (std::uint32_t index = static_cast<std::uint32_t>(threadIdx.x); index < patch_count; index += blockDim.x) {
        const auto* patch = patches + static_cast<std::size_t>(index) * kSparsePatternPatchBytes;
        const auto position = read_sparse_u32_device(patch);
        output[block.output_offset + position] = patch[sizeof(std::uint32_t)];
    }
}

// Purpose: Cooperatively checksum each fixed-size device segment without a long per-thread serial chain.
// Inputs: input/input_len and output segments are valid device storage; one 256-thread block handles each segment.
// Outputs: Writes ordered finalized CRCs and lengths with unchanged compact metadata and bounded shared storage.
__global__ void crc32_segments_kernel(const std::byte* input, std::size_t input_len, DeviceCrcSegment* segments,
                                      std::uint32_t segment_count, std::uint32_t segment_bytes) {
    const auto segment_index = static_cast<std::uint32_t>(blockIdx.x);
    if (segment_index >= segment_count) {
        return;
    }
    const auto segment_start = static_cast<std::size_t>(segment_index) * segment_bytes;
    const auto length =
        static_cast<std::uint32_t>(min(static_cast<std::size_t>(segment_bytes), input_len - segment_start));
    const auto stride = (length + kCrcSegmentThreads - 1U) / kCrcSegmentThreads;
    const auto start = min(static_cast<std::uint32_t>(threadIdx.x) * stride, length);
    const auto end = min(start + stride, length);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t pos = start; pos < end; ++pos) {
        const auto octet = static_cast<std::uint8_t>(input[segment_start + pos]);
        crc = kDeviceCrc32Table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
    }
    publish_cooperative_crc32(crc ^ 0xFFFFFFFFU, end - start, segments, segment_index);
}

struct CrcInputRange {
    std::uint64_t offset;
    std::uint32_t length;
};

// Purpose: Cooperatively checksum independent ranges without crossing file/block boundaries.
// Inputs: input/ranges are valid device storage; one 256-thread block handles each range of at most 32 KiB.
// Outputs: Writes one finalized CRC and byte count per range with bounded shared storage and stable input order.
__global__ void crc32_independent_ranges_kernel(const std::byte* input, const CrcInputRange* ranges,
                                                DeviceCrcSegment* segments, std::uint32_t segment_count) {
    const auto index = static_cast<std::uint32_t>(blockIdx.x);
    if (index >= segment_count) {
        return;
    }
    const auto range = ranges[index];
    const auto stride = (range.length + kCrcSegmentThreads - 1U) / kCrcSegmentThreads;
    const auto start = min(static_cast<std::uint32_t>(threadIdx.x) * stride, range.length);
    const auto end = min(start + stride, range.length);
    std::uint32_t checksum = 0xFFFFFFFFU;
    for (std::uint32_t pos = start; pos < end; ++pos) {
        const auto octet = static_cast<std::uint8_t>(input[range.offset + pos]);
        checksum = kDeviceCrc32Table[(checksum ^ octet) & 0xFFU] ^ (checksum >> 8U);
    }
    publish_cooperative_crc32(checksum ^ 0xFFFFFFFFU, end - start, segments, index);
}

// Purpose: Locate the decoded block that contains one output byte offset.
// Inputs: `blocks`/`block_count` describe a validated decoded layout and `output_offset` is a byte position.
// Outputs: Returns a block index less than `block_count`, or `block_count` if metadata is inconsistent.
__device__ std::uint32_t find_decoded_block(const DeviceBlock* blocks, std::uint32_t block_count,
                                            std::size_t output_offset) {
    std::uint32_t lower = 0;
    std::uint32_t upper = block_count;
    while (lower < upper) {
        const auto middle = lower + ((upper - lower) / 2U);
        const auto start = static_cast<std::size_t>(blocks[middle].output_offset);
        const auto end = start + static_cast<std::size_t>(blocks[middle].uncompressed_len);
        if (output_offset < start) {
            upper = middle;
        } else if (output_offset >= end) {
            lower = middle + 1U;
        } else {
            return middle;
        }
    }
    return block_count;
}

// Purpose: Cooperatively checksum simple decoded streams without allocating a decoded temporary buffer.
// Inputs: payload/blocks/output_len describe a validated raw/fill/pattern layout; each block handles one segment.
// Outputs: Writes ordered segment CRCs/lengths, with uniform barriers even for empty per-thread tails.
__global__ void decoded_crc32_segments_kernel(const std::byte* payload, const DeviceBlock* blocks,
                                              std::uint32_t block_count, std::size_t output_len,
                                              DeviceCrcSegment* segments, std::uint32_t segment_count,
                                              std::uint32_t segment_bytes) {
    const auto segment_index = static_cast<std::uint32_t>(blockIdx.x);
    if (segment_index >= segment_count) {
        return;
    }
    const auto segment_start = static_cast<std::size_t>(segment_index) * segment_bytes;
    const auto length =
        static_cast<std::uint32_t>(min(static_cast<std::size_t>(segment_bytes), output_len - segment_start));
    const auto stride = (length + kCrcSegmentThreads - 1U) / kCrcSegmentThreads;
    const auto start = segment_start + min(static_cast<std::uint32_t>(threadIdx.x) * stride, length);
    const auto end = segment_start + min(static_cast<std::uint32_t>(threadIdx.x + 1U) * stride, length);
    std::uint32_t crc = 0xFFFFFFFFU;
    auto block_index = find_decoded_block(blocks, block_count, start);
    std::size_t pos = start;
    while (pos < end) {
        while (block_index < block_count) {
            const auto block_end =
                static_cast<std::size_t>(blocks[block_index].output_offset) + blocks[block_index].uncompressed_len;
            if (pos < block_end) {
                break;
            }
            ++block_index;
        }
        if (block_index >= block_count) {
            break;
        }
        const auto& block = blocks[block_index];
        const auto block_end =
            min(end, static_cast<std::size_t>(block.output_offset) + static_cast<std::size_t>(block.uncompressed_len));
        if (block.kind == 1) {
            const auto octet = block.fill_value;
            while (pos < block_end) {
                crc = kDeviceCrc32Table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
                ++pos;
            }
        } else if (block.kind == 0) {
            auto payload_offset =
                static_cast<std::size_t>(block.encoded_offset) + (pos - static_cast<std::size_t>(block.output_offset));
            while (pos < block_end) {
                const auto octet = static_cast<std::uint8_t>(payload[payload_offset]);
                crc = kDeviceCrc32Table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
                ++payload_offset;
                ++pos;
            }
        } else if (block.kind == 3 && block.encoded_len != 0U) {
            const auto pattern_offset = static_cast<std::size_t>(block.encoded_offset);
            const auto pattern_len = static_cast<std::size_t>(block.encoded_len);
            while (pos < block_end) {
                const auto in_block = pos - static_cast<std::size_t>(block.output_offset);
                const auto octet = static_cast<std::uint8_t>(payload[pattern_offset + (in_block % pattern_len)]);
                crc = kDeviceCrc32Table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
                ++pos;
            }
        } else {
            break;
        }
    }
    publish_cooperative_crc32(crc ^ 0xFFFFFFFFU, static_cast<std::uint32_t>(end - start), segments, segment_index);
}

// Purpose: Run integer-heavy work over a device buffer for the standalone GPU diagnostic.
// Inputs: `data` is a device buffer of `words` 32-bit elements, `seed` changes each launch, and `rounds` controls
// arithmetic intensity. Outputs: Mutates every device word deterministically.
__global__ void diagnostic_compute_kernel(std::uint32_t* data, std::size_t words, std::uint32_t seed,
                                          std::uint32_t rounds) {
    const auto stride = static_cast<std::size_t>(blockDim.x) * gridDim.x;
    auto index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    while (index < words) {
        auto value = data[index] ^ seed ^ static_cast<std::uint32_t>(index);
        for (std::uint32_t round = 0; round < rounds; ++round) {
            value ^= value << 13U;
            value ^= value >> 17U;
            value ^= value << 5U;
            value += 0x9E3779B9U + round + static_cast<std::uint32_t>(index);
        }
        data[index] = value;
        index += stride;
    }
}

// Purpose: Reduce a device buffer into per-block checksum partials.
// Inputs: `data` is a device buffer, `words` is its length, and `partials` has one element per launched block.
// Outputs: Writes one 64-bit checksum partial per block.
__global__ void diagnostic_checksum_kernel(const std::uint32_t* data, std::size_t words, unsigned long long* partials) {
    extern __shared__ unsigned long long shared[];
    unsigned long long sum = 0;
    const auto stride = static_cast<std::size_t>(blockDim.x) * gridDim.x;
    auto index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    while (index < words) {
        sum += static_cast<unsigned long long>(data[index]);
        index += stride;
    }
    shared[threadIdx.x] = sum;
    __syncthreads();
    for (unsigned int step = blockDim.x / 2U; step > 0U; step >>= 1U) {
        if (threadIdx.x < step) {
            shared[threadIdx.x] += shared[threadIdx.x + step];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        partials[blockIdx.x] = shared[0];
    }
}

// Purpose: Select the measured small- or large-chunk CRC geometry without changing checksum semantics.
// Inputs: `bytes` is the total source or decoded byte count for one operation.
// Outputs: Returns a bounded 8 KiB or 32 KiB segment size used consistently by host and HIP code.
std::uint32_t crc_segment_bytes_for_size(std::uint64_t bytes) {
    return bytes <= kSmallCrcInputLimitBytes ? kSmallCrcSegmentBytes : kLargeCrcSegmentBytes;
}

// Purpose: Return the number of selected-size CRC segments needed for a device buffer.
// Inputs: `bytes` is the buffer length and `segment_bytes` is the operation's selected geometry.
// Outputs: Returns zero for empty buffers or a bounded segment count for nonempty buffers.
std::uint32_t crc_segment_count(std::uint64_t bytes, std::uint32_t segment_bytes) {
    if (bytes == 0) {
        return 0;
    }
    const auto segments = (bytes + segment_bytes - 1U) / segment_bytes;
    if (segments > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("CRC segment count exceeds HIP launch limits");
    }
    return static_cast<std::uint32_t>(segments);
}

// Purpose: Combine ordered CRC segments into one ZIP-compatible finalized CRC-32.
// Inputs: `segments` contains finalized per-segment CRCs and byte lengths in archive order.
// Outputs: Returns the finalized CRC for the concatenated byte stream.
std::uint32_t combine_crc_segments(std::span<const DeviceCrcSegment> segments) {
    std::uint32_t combined = 0;
    for (const auto& segment : segments) {
        combined = crc32_combine(combined, segment.crc32, segment.length);
    }
    return combined;
}

// Purpose: Launch the HIP CRC segment kernel and return the combined CRC.
// Inputs: `device_input` is an allocated device buffer, `input_len` is its byte length, and `telemetry` records HIP
// activity. Outputs: Returns the finalized CRC-32 while copying only compact segment metadata back to host memory.
std::uint32_t compute_crc32_device(const std::byte* device_input, std::uint64_t input_len, GpuTelemetry* telemetry,
                                   const char* action) {
    if (input_len == 0) {
        return 0;
    }
    const auto crc_segment_bytes = crc_segment_bytes_for_size(input_len);
    const auto segments = crc_segment_count(input_len, crc_segment_bytes);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    HipDeviceMemoryReservation reservation(segment_bytes, action);
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = segments;
    auto events = make_hip_event_pair("create crc32_segments_kernel events");
    launch_measured_kernel(crc32_segments_kernel, grid, threads, 0, hipStreamPerThread, events,
                           "launch crc32_segments_kernel", device_input, static_cast<std::size_t>(input_len),
                           device_segments.get(), segments, crc_segment_bytes);
    finish_measured_kernel(telemetry, events, "synchronize crc32_segments_kernel");

    std::vector<DeviceCrcSegment> host_segments(segments);
    check_hip(copy_on_codec_stream(host_segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy CRC segments");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    device_segments.reset_checked("hipFree CRC segments");
    return combine_crc_segments(host_segments);
}

// Purpose: Calculate a GPU source CRC for each independent block in a validated bounded batch.
// Inputs: device_input contains the concatenated blocks; lengths define exact boundaries; telemetry records costs.
// Outputs: Returns ordered block CRCs after one checksum launch; copies back metadata only and releases all scratch.
std::vector<std::uint32_t> compute_block_crc32_device(const std::byte* device_input,
                                                      std::span<const std::uint32_t> lengths, GpuTelemetry* telemetry) {
    std::vector<CrcInputRange> ranges;
    std::size_t offset = 0;
    const auto total_input_bytes = std::accumulate(lengths.begin(), lengths.end(), std::uint64_t{0});
    const auto crc_segment_bytes = crc_segment_bytes_for_size(total_input_bytes);
    for (const auto length : lengths) {
        for (std::uint32_t pos = 0; pos < length;) {
            const auto count = std::min(crc_segment_bytes, length - pos);
            ranges.push_back(CrcInputRange{.offset = offset + pos, .length = count});
            pos += count;
        }
        offset += length;
    }
    if (ranges.empty()) {
        return {};
    }
    const auto range_bytes = checked_multiply_bytes(ranges.size(), sizeof(CrcInputRange), "batch CRC ranges");
    const auto segment_bytes = checked_multiply_bytes(ranges.size(), sizeof(DeviceCrcSegment), "batch CRC results");
    const auto total_bytes = checked_add_bytes(range_bytes, segment_bytes, "batch CRC scratch");
    HipDeviceMemoryReservation reservation(total_bytes, "batch CRC scratch");
    HipDeviceBuffer<CrcInputRange> device_ranges(range_bytes, "hipMalloc batch CRC ranges");
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc batch CRC results");
    record_gpu_device_allocation_bytes(telemetry, total_bytes);
    check_hip(copy_on_codec_stream(device_ranges.get(), ranges.data(), range_bytes, hipMemcpyHostToDevice),
              "hipMemcpy batch CRC ranges");
    record_gpu_h2d_bytes(telemetry, range_bytes);
    const auto count = static_cast<std::uint32_t>(ranges.size());
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = count;
    auto events = make_hip_event_pair("create independent CRC events");
    launch_measured_kernel(crc32_independent_ranges_kernel, grid, threads, 0, hipStreamPerThread, events,
                           "launch independent CRC kernel", device_input, device_ranges.get(), device_segments.get(),
                           count);
    finish_measured_kernel(telemetry, events, "synchronize independent CRC kernel");
    std::vector<DeviceCrcSegment> segments(count);
    check_hip(copy_on_codec_stream(segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy independent CRC results");
    record_gpu_d2h_bytes(telemetry, segment_bytes);
    device_ranges.reset_checked("hipFree batch CRC ranges");
    device_segments.reset_checked("hipFree batch CRC results");
    std::vector<std::uint32_t> checksums;
    checksums.reserve(lengths.size());
    std::size_t first = 0;
    for (const auto length : lengths) {
        const auto segment_count = crc_segment_count(length, crc_segment_bytes);
        checksums.push_back(
            combine_crc_segments(std::span<const DeviceCrcSegment>(segments).subspan(first, segment_count)));
        first += segment_count;
    }
    return checksums;
}

// Purpose: Launch the HIP decoded-stream CRC kernel without materializing decoded output.
// Inputs: `device_payload`/`device_blocks` are allocated device buffers, `block_count` and `output_len` bound the
// decoded layout, and `telemetry` records HIP activity. Outputs: Returns the finalized CRC-32 while copying only
// compact segment metadata back to host memory.
std::uint32_t compute_decoded_crc32_device(const std::byte* device_payload, const DeviceBlock* device_blocks,
                                           std::uint32_t block_count, std::uint64_t output_len, GpuTelemetry* telemetry,
                                           const char* action) {
    if (output_len == 0) {
        return 0;
    }
    const auto crc_segment_bytes = crc_segment_bytes_for_size(output_len);
    const auto segments = crc_segment_count(output_len, crc_segment_bytes);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    HipDeviceMemoryReservation reservation(segment_bytes, action);
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc decoded CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = segments;
    auto events = make_hip_event_pair("create decoded_crc32_segments_kernel events");
    launch_measured_kernel(decoded_crc32_segments_kernel, grid, threads, 0, hipStreamPerThread, events,
                           "launch decoded_crc32_segments_kernel", device_payload, device_blocks, block_count,
                           static_cast<std::size_t>(output_len), device_segments.get(), segments, crc_segment_bytes);
    finish_measured_kernel(telemetry, events, "synchronize decoded_crc32_segments_kernel");

    std::vector<DeviceCrcSegment> host_segments(segments);
    check_hip(copy_on_codec_stream(host_segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy decoded CRC segments");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    device_segments.reset_checked("hipFree decoded CRC segments");
    return combine_crc_segments(host_segments);
}

// Purpose: Build GPU decode block metadata with contiguous decoded output offsets.
// Inputs: `blocks` is the validated archive block table.
// Outputs: Returns device-ready block metadata in archive order.
std::vector<DeviceBlock> build_decode_device_blocks(std::span<const BlockDescriptor> blocks) {
    std::vector<DeviceBlock> host_blocks;
    host_blocks.reserve(blocks.size());
    std::uint64_t output_offset = 0;
    for (const auto& block : blocks) {
        host_blocks.push_back(DeviceBlock{
            .kind = static_cast<std::uint8_t>(block.kind),
            .fill_value = block.fill_value,
            .reserved = 0,
            .uncompressed_len = block.uncompressed_len,
            .encoded_offset = block.encoded_offset,
            .output_offset = output_offset,
            .encoded_len = block.encoded_len,
        });
        output_offset += block.uncompressed_len;
    }
    return host_blocks;
}

// Purpose: Count the independently decoded prefix segments in one validated device block.
// Inputs: `block` has bounded decoded length and a recognized native block kind.
// Outputs: Returns zero for other kinds or the exact static/adaptive/Huffman segment count.
std::uint32_t prefix_decode_segment_count(const DeviceBlock& block) {
    if (block.kind != static_cast<std::uint8_t>(BlockKind::GpuPrefix) &&
        block.kind != static_cast<std::uint8_t>(BlockKind::GpuAdaptivePrefix) &&
        block.kind != static_cast<std::uint8_t>(BlockKind::GpuHuffman)) {
        return 0U;
    }
    return (block.uncompressed_len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
}

// Purpose: Build GPU decode plans with one exact allocation for every prefix segment in a decoded chunk.
// Inputs: `host_blocks` is the validated device-block table with decoded output offsets.
// Outputs: Returns ordered 4 KiB plans without per-block vector growth.
std::vector<PrefixDecodeSegment> build_prefix_decode_segments(std::span<const DeviceBlock> host_blocks) {
    std::vector<PrefixDecodeSegment> plans;
    // A validated segment count cannot exceed the chunk's bounded decoded byte count.
    const auto plan_count = std::accumulate(
        host_blocks.begin(), host_blocks.end(), std::size_t{0},
        [](std::size_t total, const DeviceBlock& block) { return total + prefix_decode_segment_count(block); });
    plans.reserve(plan_count);
    for (const auto& block : host_blocks) {
        const auto segment_count = prefix_decode_segment_count(block);
        if (segment_count == 0U) {
            continue;
        }
        const bool adaptive = block.kind == static_cast<std::uint8_t>(BlockKind::GpuAdaptivePrefix);
        const bool huffman = block.kind == static_cast<std::uint8_t>(BlockKind::GpuHuffman);
        const auto table_offset =
            block.encoded_offset + (huffman    ? static_cast<std::uint64_t>(kGpuHuffmanLookupBytes)
                                    : adaptive ? static_cast<std::uint64_t>(kGpuAdaptivePrefixCodebookBytes)
                                               : 0U);
        const auto table_bytes = static_cast<std::uint64_t>(segment_count + 1U) * sizeof(std::uint32_t);
        for (std::uint32_t segment = 0; segment < segment_count; ++segment) {
            const auto decoded_offset = static_cast<std::uint64_t>(segment) * kGpuPrefixSegmentBytes;
            const auto remaining = block.uncompressed_len - static_cast<std::uint32_t>(decoded_offset);
            plans.push_back(PrefixDecodeSegment{
                .codebook_offset = block.encoded_offset,
                .table_offset = table_offset,
                .bitstream_offset = table_offset + table_bytes,
                .output_offset = block.output_offset + decoded_offset,
                .segment_index = segment,
                .decoded_len = std::min<std::uint32_t>(kGpuPrefixSegmentBytes, remaining),
                .adaptive = huffman    ? 2U
                            : adaptive ? 1U
                                       : 0U,
            });
        }
    }
    return plans;
}

// Purpose: Collect dictionary spans directly into one reserved private device-buffer plan.
// Inputs: `payload` is the complete encoded chunk and `blocks` contains decoded output offsets.
// Outputs: Returns bounded non-overlapping spans for the shared HIP dictionary decoder.
std::vector<DictionarySegmentSpan> build_dictionary_decode_segments(std::span<const std::byte> payload,
                                                                    std::span<const DeviceBlock> blocks) {
    std::vector<DictionarySegmentSpan> plans;
    // Validated decoded extents bound the complete plan count before scratch allocation.
    const auto plan_count =
        std::accumulate(blocks.begin(), blocks.end(), std::size_t{0}, [](std::size_t total, const DeviceBlock& block) {
            return total + (block.kind == static_cast<std::uint8_t>(BlockKind::GpuDictionary)
                                ? (static_cast<std::size_t>(block.uncompressed_len) + kGpuDictionarySegmentBytes - 1U) /
                                      kGpuDictionarySegmentBytes
                                : 0U);
        });
    plans.reserve(plan_count);
    for (const auto& block : blocks) {
        if (block.kind != static_cast<std::uint8_t>(BlockKind::GpuDictionary)) {
            continue;
        }
        const auto block_payload = payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
        const auto begin = plans.size();
        scan_dictionary_segments(block_payload, block.uncompressed_len, &plans);
        for (std::size_t index = begin; index < plans.size(); ++index) {
            plans[index].encoded_offset += static_cast<std::uint32_t>(block.encoded_offset);
            plans[index].decoded_offset += static_cast<std::uint32_t>(block.output_offset);
        }
    }
    return plans;
}

// Purpose: Launch the standard raw/fill/pattern materializer only when it has work.
// Inputs: `device_payload`, `device_blocks`, `host_blocks`, `device_output`, and `output_len` describe the decode job.
// Outputs: Writes non-prefix decoded bytes or returns without a kernel launch for prefix-only chunks.
void materialize_non_prefix_segments_device(const std::byte* device_payload, const DeviceBlock* device_blocks,
                                            std::span<const DeviceBlock> host_blocks, std::byte* device_output,
                                            std::size_t output_len, GpuTelemetry* telemetry) {
    if (!has_non_prefix_materialization_blocks(host_blocks)) {
        return;
    }
    constexpr int threads = 256;
    const auto segments = (output_len + kMaterializeSegmentBytes - 1U) / kMaterializeSegmentBytes;
    if (segments > std::numeric_limits<unsigned int>::max()) {
        throw GpuError("decode materialize segment count exceeds HIP launch limits");
    }
    auto events = make_hip_event_pair("create materialize_segments_kernel events");
    launch_measured_kernel(materialize_segments_kernel, static_cast<unsigned int>(segments), threads, 0,
                           hipStreamPerThread, events, "launch materialize_segments_kernel", device_payload,
                           device_blocks, static_cast<std::uint32_t>(host_blocks.size()), device_output, output_len);
    finish_measured_kernel(telemetry, events, "synchronize materialize_segments_kernel");
}

// Purpose: Finish sparse blocks after every independent motif segment has been materialized.
// Inputs: Device payload/block/output buffers and the validated host block table share the same live allocation.
// Outputs: Applies canonical corrections on HIP, or skips the launch when no sparse block exists.
void materialize_sparse_patches_device(const std::byte* device_payload, const DeviceBlock* device_blocks,
                                       std::span<const DeviceBlock> host_blocks, std::byte* device_output,
                                       GpuTelemetry* telemetry) {
    if (!std::ranges::any_of(host_blocks, [](const DeviceBlock& block) {
            return is_gpu_sparse_pattern_kind(static_cast<BlockKind>(block.kind));
        })) {
        return;
    }
    if (host_blocks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("sparse decode block count exceeds HIP launch limits");
    }
    auto events = make_hip_event_pair("create sparse patch events");
    launch_measured_kernel(apply_sparse_patches_kernel, static_cast<unsigned int>(host_blocks.size()), 256, 0,
                           hipStreamPerThread, events, "launch sparse patch kernel", device_payload, device_blocks,
                           device_output, static_cast<std::uint32_t>(host_blocks.size()));
    finish_measured_kernel(telemetry, events, "synchronize sparse patch kernel");
}

// Purpose: Launch GPU prefix materialization for the prefix-coded portions of one decoded chunk.
// Inputs: `device_payload`, `device_output`, and `plans` describe already validated prefix segments.
// Outputs: Writes decoded prefix bytes into `device_output`; throws on HIP errors.
void materialize_prefix_segments_device(const std::byte* device_payload, std::byte* device_output,
                                        std::span<const PrefixDecodeSegment> plans, GpuTelemetry* telemetry) {
    if (plans.empty()) {
        return;
    }
    if (plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU prefix decode segment count exceeds HIP launch limits");
    }
    const auto plan_bytes = checked_multiply_bytes(plans.size(), sizeof(PrefixDecodeSegment), "prefix decode plans");
    const auto error_bytes = checked_multiply_bytes(plans.size(), sizeof(std::uint32_t), "prefix decode errors");
    const auto required_bytes = checked_add_bytes(plan_bytes, error_bytes, "prefix decode metadata");
    HipDeviceMemoryReservation reservation(required_bytes, "prefix decode metadata");
    HipDeviceBuffer<PrefixDecodeSegment> device_plans(plan_bytes, "hipMalloc prefix decode plans");
    HipDeviceBuffer<std::uint32_t> device_errors(error_bytes, "hipMalloc prefix decode errors");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(copy_on_codec_stream(device_plans.get(), plans.data(), plan_bytes, hipMemcpyHostToDevice),
              "hipMemcpy prefix decode plans");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes));
    auto events = make_hip_event_pair("create materialize_prefix_segments_kernel events");
    // Two lanes reduced gfx1201 decode time; 32- and 64-lane launches regressed on the measured workload.
    constexpr unsigned int kDecodeThreads = 2U;
    const auto decode_blocks = (plans.size() + kDecodeThreads - 1U) / kDecodeThreads;
    launch_measured_kernel(materialize_prefix_segments_kernel, static_cast<unsigned int>(decode_blocks), kDecodeThreads,
                           0, hipStreamPerThread, events, "launch materialize_prefix_segments_kernel", device_payload,
                           device_plans.get(), static_cast<std::uint32_t>(plans.size()), device_output,
                           device_errors.get());
    finish_measured_kernel(telemetry, events, "synchronize materialize_prefix_segments_kernel");
    std::vector<std::uint32_t> errors(plans.size());
    check_hip(copy_on_codec_stream(errors.data(), device_errors.get(), error_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy prefix decode errors");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(error_bytes));
    if (std::ranges::any_of(errors, [](std::uint32_t error) { return error != 0U; })) {
        throw ArchiveError("GPU prefix block contains a truncated or invalid codeword");
    }
    device_errors.reset_checked("hipFree prefix decode errors");
    device_plans.reset_checked("hipFree prefix decode plans");
}

// Purpose: Verify decoded bytes for compressed HIP blocks without copying the decoded chunk back to the host.
// Inputs: `payload`, `blocks`, `output_size`, and `options` describe one validated decoded chunk.
// Outputs: Returns a finalized CRC-32 computed from a GPU-materialized output buffer.
std::uint32_t compute_materialized_crc32_device(std::span<const std::byte> payload,
                                                std::span<const BlockDescriptor> blocks, std::uint64_t output_size,
                                                const GpuCodecOptions& options) {
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    auto host_blocks = build_decode_device_blocks(blocks);
    const auto prefix_plans = build_prefix_decode_segments(host_blocks);
    const auto dictionary_plans = build_dictionary_decode_segments(payload, host_blocks);
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "prefix CRC decode block table");
    auto required_bytes =
        checked_add_bytes(payload_bytes, static_cast<std::size_t>(output_size), "prefix CRC decode memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "prefix CRC decode memory");
    HipDeviceMemoryReservation reservation(required_bytes, "prefix CRC decode");
    HipDeviceBuffer<std::byte> device_payload(payload_bytes, "hipMalloc prefix CRC payload");
    HipDeviceBuffer<std::byte> device_output(static_cast<std::size_t>(output_size), "hipMalloc prefix CRC output");
    HipDeviceBuffer<DeviceBlock> device_blocks(block_table_bytes, "hipMalloc prefix CRC blocks");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    if (!payload.empty()) {
        check_hip(copy_on_codec_stream(device_payload.get(), payload.data(), payload.size(), hipMemcpyHostToDevice),
                  "hipMemcpy prefix CRC payload");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
    }
    check_hip(copy_on_codec_stream(device_blocks.get(), host_blocks.data(), block_table_bytes, hipMemcpyHostToDevice),
              "hipMemcpy prefix CRC blocks");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));
    materialize_non_prefix_segments_device(device_payload.get(), device_blocks.get(), host_blocks, device_output.get(),
                                           static_cast<std::size_t>(output_size), telemetry);
    materialize_sparse_patches_device(device_payload.get(), device_blocks.get(), host_blocks, device_output.get(),
                                      telemetry);
    materialize_prefix_segments_device(device_payload.get(), device_output.get(), prefix_plans, telemetry);
    dictionary::decode_segments_device(device_payload.get(), dictionary_plans, device_output.get(), telemetry);
    const auto crc = compute_crc32_device(device_output.get(), output_size, telemetry, "prefix decoded CRC");
    device_payload.reset_checked("hipFree prefix CRC payload");
    device_output.reset_checked("hipFree prefix CRC output");
    device_blocks.reset_checked("hipFree prefix CRC blocks");
    return crc;
}

// Purpose: Mark a sequential HIP encode phase boundary using host steady-clock time.
// Inputs: Optional telemetry, the completed phase, and its mutable start point.
// Outputs: Accumulates worker time and advances the start point to the next phase.
void record_encode_phase(GpuTelemetry* telemetry, GpuEncodeStage stage,
                         std::chrono::steady_clock::time_point& started) {
    const auto finished = std::chrono::steady_clock::now();
    record_gpu_encode_stage_time(telemetry, stage, finished - started);
    started = finished;
}

// Purpose: Mark nested classification boundaries without resetting the enclosing encode-phase clock.
// Inputs: Optional telemetry, the completed substage, and its mutable steady-clock start point.
// Outputs: Records worker time and advances only the nested start point; no HIP synchronization is added.
void record_classification_phase(GpuTelemetry* telemetry, GpuClassificationStage stage,
                                 std::chrono::steady_clock::time_point& started) {
    const auto finished = std::chrono::steady_clock::now();
    record_gpu_classification_stage_time(telemetry, stage, finished - started);
    started = finished;
}

}  // namespace

// Purpose: Run a HIP-only workload that proves the AMD GPU can execute sustained kernels.
// Inputs: `options` controls wall duration, device buffer size, and integer work per launch.
// Outputs: Returns HIP event timing, transfer/allocation counters, and a device-produced checksum.
GpuDiagnosticResult run_gpu_diagnostic_hip(const GpuDiagnosticOptions& options) {
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto bytes = static_cast<std::uint64_t>(options.buffer_mib) * 1024ULL * 1024ULL;
    if ((bytes % sizeof(std::uint32_t)) != 0) {
        throw GpuError("diagnostic buffer must be divisible by uint32 size");
    }
    const auto words = static_cast<std::size_t>(bytes / sizeof(std::uint32_t));
    constexpr int threads = 256;
    const int blocks = 1024;
    const auto partial_bytes = checked_multiply_bytes(blocks, sizeof(unsigned long long), "diagnostic partials");
    const auto required_bytes =
        checked_add_bytes(static_cast<std::size_t>(bytes), partial_bytes, "diagnostic device memory");
    HipDeviceMemoryReservation reservation(required_bytes, "diagnostic");

    std::vector<std::uint32_t> host(words);
    for (std::size_t i = 0; i < host.size(); ++i) {
        host[i] = static_cast<std::uint32_t>(i * 2654435761U);
    }

    HipDeviceBuffer<std::uint32_t> device_data(static_cast<std::size_t>(bytes), "hipMalloc diagnostic data");
    HipDeviceBuffer<unsigned long long> device_partials(partial_bytes, "hipMalloc diagnostic partials");
    {
        check_hip(copy_on_codec_stream(device_data.get(), host.data(), static_cast<std::size_t>(bytes),
                                       hipMemcpyHostToDevice),
                  "hipMemcpy diagnostic input");

        GpuDiagnosticResult result;
        GpuTelemetry timing;
        result.info = info;
        result.bytes = bytes;
        result.h2d_bytes = bytes;
        result.device_allocation_bytes = static_cast<std::uint64_t>(required_bytes);

        const auto started = std::chrono::steady_clock::now();
        std::uint32_t seed = 0xA5A5A5A5U;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < options.seconds) {
            auto events = make_hip_event_pair("create diagnostic_compute_kernel events");
            launch_measured_kernel(diagnostic_compute_kernel, blocks, threads, 0, hipStreamPerThread, events,
                                   "launch diagnostic_compute_kernel", device_data.get(), words, seed,
                                   options.inner_iterations);
            finish_measured_kernel(&timing, events, "synchronize diagnostic_compute_kernel");
            seed += 0x9E3779B9U;
        }

        auto checksum_events = make_hip_event_pair("create diagnostic_checksum_kernel events");
        launch_measured_kernel(diagnostic_checksum_kernel, blocks, threads, threads * sizeof(unsigned long long),
                               hipStreamPerThread, checksum_events, "launch diagnostic_checksum_kernel",
                               device_data.get(), words, device_partials.get());
        finish_measured_kernel(&timing, checksum_events, "synchronize diagnostic_checksum_kernel");
        const auto timing_stats = snapshot_gpu_telemetry(timing);
        result.kernel_ms = timing_stats.kernel_ms;
        result.kernel_launches = timing_stats.kernel_launches;

        std::vector<unsigned long long> partials(blocks);
        check_hip(copy_on_codec_stream(partials.data(), device_partials.get(), partial_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy diagnostic partials");
        result.d2h_bytes = static_cast<std::uint64_t>(partial_bytes);
        result.checksum = std::accumulate(partials.begin(), partials.end(), 0ULL);
        result.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        device_data.reset_checked("hipFree diagnostic data");
        device_partials.reset_checked("hipFree diagnostic partials");
        return result;
    }
}

enum class HipEncodeRole { Source, CompoundTrial };

// Purpose: Declare the bounded HIP encoder shared by source blocks and nonrecursive composition trials.
// Inputs: Source ownership, GPU policy, optional exact block lengths and checksum destinations.
// Outputs: Returns GPU payloads; role bounds composition depth and source-kind/phase accounting.
EncodedChunk encode_chunk_hip_impl(std::span<const std::byte> input, std::vector<std::byte>* owned_input,
                                   const GpuCodecOptions& options, std::span<const std::uint32_t> block_lengths = {},
                                   std::vector<std::uint32_t>* block_crcs = nullptr,
                                   HipEncodeRole role = HipEncodeRole::Source);

// Purpose: Evaluate exactly one extra GPU encoding stage over each eligible Neutron winner.
// Inputs: A dense immutable baseline and required-HIP Neutron policy; ordinary levels never call this helper.
// Outputs: Returns only strictly smaller complete compound frames, preserving original bytes on losses and ties.
EncodedChunk compose_neutron_winners(EncodedChunk baseline, const GpuCodecOptions& options) {
    if (std::ranges::none_of(baseline.blocks, [](const BlockDescriptor& block) {
            return is_gpu_compound_stage(block.kind) && block.encoded_len > kGpuCompoundHeaderBytes;
        })) {
        return baseline;
    }
    EncodedChunk result;
    result.source_crc32 = baseline.source_crc32;
    result.source_crc32_available = baseline.source_crc32_available;
    result.gpu_used = baseline.gpu_used;
    result.blocks.reserve(baseline.blocks.size());
    result.payload.reserve(baseline.payload.size());
    auto inner_options = options;
    inner_options.compression_mode = NativeCompressionMode::NeutronStar;
    inner_options.compression_level = 9;
    for (auto descriptor : baseline.blocks) {
        const auto offset = static_cast<std::size_t>(descriptor.encoded_offset);
        if (offset > baseline.payload.size() || descriptor.encoded_len > baseline.payload.size() - offset) {
            throw GpuError("Neutron composition baseline exceeds its payload");
        }
        const auto original = std::span(baseline.payload).subspan(offset, descriptor.encoded_len);
        descriptor.encoded_offset = result.payload.size();
        bool selected = false;
        if (is_gpu_compound_stage(descriptor.kind) && original.size() > kGpuCompoundHeaderBytes) {
            if (original.size() >= descriptor.uncompressed_len) {
                throw GpuError("Neutron composition requires a smaller original encoding");
            }
            if (options.encode_checkpoint) {
                options.encode_checkpoint();
            }
            (void)resolve_host_pipeline_inflight_limit(query_host_memory_snapshot(), descriptor.uncompressed_len);
            const auto inner =
                encode_chunk_hip_impl(original, nullptr, inner_options, {}, nullptr, HipEncodeRole::CompoundTrial);
            if (inner.blocks.size() != 1U) {
                throw GpuError("Neutron composition must produce exactly one inner block");
            }
            const auto& stage = inner.blocks.front();
            if (stage.uncompressed_len != original.size() || stage.encoded_offset != 0U ||
                stage.encoded_len != inner.payload.size() || !inner.source_crc32_available) {
                throw GpuError("Neutron secondary encoder returned inconsistent stage metadata");
            }
            if ((is_gpu_compound_stage(stage.kind) || stage.kind == BlockKind::Fill) &&
                inner.payload.size() < original.size() - kGpuCompoundHeaderBytes) {
                result.payload.push_back(static_cast<std::byte>(stage.kind));
                result.payload.push_back(static_cast<std::byte>(descriptor.kind));
                result.payload.push_back(stage.kind == BlockKind::Fill ? static_cast<std::byte>(stage.fill_value)
                                                                       : std::byte{0});
                result.payload.push_back(std::byte{0});
                for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
                    result.payload.push_back(static_cast<std::byte>(descriptor.encoded_len >> (byte * 8U)));
                }
                result.payload.insert(result.payload.end(), inner.payload.begin(), inner.payload.end());
                descriptor.kind = BlockKind::GpuCompound;
                descriptor.fill_value = 0U;
                descriptor.encoded_len = static_cast<std::uint32_t>(kGpuCompoundHeaderBytes + inner.payload.size());
                (void)parse_gpu_compound_block(
                    std::span(result.payload)
                        .subspan(static_cast<std::size_t>(descriptor.encoded_offset), descriptor.encoded_len),
                    descriptor);
                selected = true;
            }
        }
        if (!selected) {
            result.payload.insert(result.payload.end(), original.begin(), original.end());
        }
        result.blocks.push_back(descriptor);
    }
    return result;
}

// Purpose: Encode independent byte-plane contexts with the existing plain HIP portfolio.
// Inputs: Transposed source, admitted width, exclusive complete-frame limit and operation-owned HIP policy.
// Outputs: Returns a strictly competitive dense frame or an empty proven loss; device errors remain errors.
static std::vector<std::byte> encode_neutron_plane_contexts(std::span<const std::byte> transformed, std::uint8_t width,
                                                            std::size_t exclusive_limit,
                                                            const GpuCodecOptions& options) {
    if (!is_gpu_byte_plane_width(width) || transformed.size() < width || transformed.size() > kMaxArchiveBlockBytes ||
        exclusive_limit > transformed.size()) {
        throw GpuError("Neutron byte-plane context geometry is invalid");
    }
    const auto header_bytes = kGpuBytePlaneHeaderBytes + width * kGpuBytePlaneContextRecordBytes;
    if (exclusive_limit <= header_bytes) {
        return {};
    }
    std::vector<std::byte> frame(header_bytes, std::byte{0});
    frame.reserve(exclusive_limit);
    frame[0] = static_cast<std::byte>(width);
    const auto records = transformed.size() / width;
    std::size_t cursor = 0U;
    for (std::size_t plane = 0U; plane < width; ++plane) {
        const auto bytes = records + (plane + 1U == width ? transformed.size() % width : 0U);
        auto trial_options = options;
        trial_options.block_size = static_cast<std::uint32_t>(bytes);
        const auto trial = encode_chunk_hip_impl(transformed.subspan(cursor, bytes), nullptr, trial_options, {},
                                                 nullptr, HipEncodeRole::CompoundTrial);
        if (trial.blocks.size() != 1U || trial.blocks.front().uncompressed_len != bytes ||
            trial.blocks.front().encoded_offset != 0U || trial.blocks.front().encoded_len != trial.payload.size()) {
            throw GpuError("Neutron byte-plane context trial returned inconsistent metadata");
        }
        const auto& inner = trial.blocks.front();
        if (!is_gpu_compound_stage(inner.kind) && inner.kind != BlockKind::Raw && inner.kind != BlockKind::Fill) {
            throw GpuError("Neutron byte-plane context trial returned a recursive or CPU codec");
        }
        if (trial.payload.size() >= exclusive_limit - frame.size()) {
            return {};
        }
        const auto offset = kGpuBytePlaneHeaderBytes + plane * kGpuBytePlaneContextRecordBytes;
        frame[offset] = static_cast<std::byte>(inner.kind);
        frame[offset + 1U] = inner.kind == BlockKind::Fill ? static_cast<std::byte>(inner.fill_value) : std::byte{0};
        for (std::size_t byte = 0U; byte < sizeof(inner.encoded_len); ++byte) {
            frame[offset + 2U + byte] = static_cast<std::byte>((inner.encoded_len >> (byte * 8U)) & 0xFFU);
        }
        frame.insert(frame.end(), trial.payload.begin(), trial.payload.end());
        cursor += bytes;
    }
    return frame;
}

// Purpose: Compare whole-block and independent-context byte-plane trials against retained Neutron winners.
// Inputs: Owned baseline/source spans and required-HIP options; trials are closed, serial and completely framed.
// Outputs: Preserves every preceding winner on ties/losses and emits only strictly smaller GPU-native frames.
static EncodedChunk select_neutron_byte_planes(std::span<const std::byte> source, EncodedChunk baseline,
                                               const GpuCodecOptions& options) {
    EncodedChunk result;
    result.source_crc32 = baseline.source_crc32;
    result.source_crc32_available = baseline.source_crc32_available;
    result.gpu_used = baseline.gpu_used;
    result.blocks.reserve(baseline.blocks.size());
    result.payload.reserve(baseline.payload.size());
    std::size_t source_offset = 0U;
    for (auto descriptor : baseline.blocks) {
        const auto offset = static_cast<std::size_t>(descriptor.encoded_offset);
        if (descriptor.uncompressed_len > source.size() - source_offset || offset > baseline.payload.size() ||
            descriptor.encoded_len > baseline.payload.size() - offset) {
            throw GpuError("Neutron byte-plane baseline exceeds its owned spans");
        }
        const auto original = source.subspan(source_offset, descriptor.uncompressed_len);
        const auto baseline_payload =
            std::span<const std::byte>(baseline.payload).subspan(offset, descriptor.encoded_len);
        auto best_bytes = baseline_payload.size();
        std::vector<std::byte> selected_frame;
        if (descriptor.kind != BlockKind::Fill && best_bytes > kGpuBytePlaneHeaderBytes) {
            (void)resolve_host_pipeline_inflight_limit(query_host_memory_snapshot(), original.size());
            std::vector<std::byte> transformed(original.size());
            auto trial_options = options;
            trial_options.block_size = descriptor.uncompressed_len;
            for (const std::uint8_t width : {2U, 4U, 8U}) {
                if (original.size() < width) {
                    continue;
                }
                transform_byte_planes_hip(original, transformed, width, false, options);
                if (width == 2U) {
                    auto contexts = encode_neutron_plane_contexts(transformed, width, best_bytes, options);
                    if (!contexts.empty()) {
                        selected_frame = std::move(contexts);
                        descriptor.kind = BlockKind::GpuBytePlaneContexts;
                        descriptor.fill_value = 0U;
                        descriptor.encoded_len = static_cast<std::uint32_t>(selected_frame.size());
                        (void)parse_gpu_byte_plane_contexts(selected_frame, descriptor);
                        best_bytes = selected_frame.size();
                    }
                }
                const auto trial = encode_chunk_hip_impl(transformed, nullptr, trial_options, {}, nullptr,
                                                         HipEncodeRole::CompoundTrial);
                if (trial.blocks.size() != 1U || trial.blocks.front().uncompressed_len != original.size() ||
                    trial.blocks.front().encoded_offset != 0U ||
                    trial.blocks.front().encoded_len != trial.payload.size()) {
                    throw GpuError("Neutron byte-plane trial returned inconsistent metadata");
                }
                const auto& inner = trial.blocks.front();
                if ((!is_gpu_compound_stage(inner.kind) && inner.kind != BlockKind::Fill) ||
                    trial.payload.size() >= best_bytes - kGpuBytePlaneHeaderBytes) {
                    continue;
                }
                selected_frame.clear();
                selected_frame.reserve(kGpuBytePlaneHeaderBytes + trial.payload.size());
                selected_frame.insert(
                    selected_frame.end(),
                    {static_cast<std::byte>(width), static_cast<std::byte>(inner.kind),
                     inner.kind == BlockKind::Fill ? static_cast<std::byte>(inner.fill_value) : std::byte{0},
                     std::byte{0}});
                selected_frame.insert(selected_frame.end(), trial.payload.begin(), trial.payload.end());
                descriptor.kind = BlockKind::GpuBytePlane;
                descriptor.fill_value = 0U;
                descriptor.encoded_len = static_cast<std::uint32_t>(selected_frame.size());
                (void)parse_gpu_byte_plane_block(selected_frame, descriptor);
                best_bytes = selected_frame.size();
            }
        }
        descriptor.encoded_offset = result.payload.size();
        const auto selected_payload =
            selected_frame.empty() ? baseline_payload : std::span<const std::byte>(selected_frame);
        result.payload.insert(result.payload.end(), selected_payload.begin(), selected_payload.end());
        result.blocks.push_back(descriptor);
        source_offset += original.size();
    }
    if (source_offset != source.size()) {
        throw GpuError("Neutron byte-plane baseline does not cover its source");
    }
    return result;
}

// Purpose: Release completed Neutron trial storage and enforce the fixed source/trial composition depth.
// Inputs: A fully owned winner, required-HIP policy/role and candidate buffers no longer borrowed by the winner.
// Outputs: Returns the winner or a smaller composed result; trial roles never start a recursive composition.
static EncodedChunk finish_neutron_encoding(std::span<const std::byte> source, EncodedChunk selected,
                                            const GpuCodecOptions& options, HipEncodeRole role,
                                            dictionary::DictionaryReplacements& dictionary_replacements,
                                            std::vector<std::vector<std::byte>>& sparse_replacements) {
    // The selected payload owns copies; releasing trial storage cannot invalidate either stage's source span.
    dictionary_replacements.clear();
    sparse_replacements.clear();
    if (role != HipEncodeRole::Source) {
        return selected;
    }
    return select_neutron_byte_planes(source, compose_neutron_winners(std::move(selected), options), options);
}

// Purpose: Compute source integrity once for ordinary chunks or independently framed dense batches.
// Inputs: A bounded device source, exact optional block lengths and operation-owned CRC destinations/telemetry.
// Outputs: Returns device-computed source CRC, combining independent batch block CRCs in source order.
static std::uint32_t encoded_source_crc_hip(const std::byte* device_input, std::size_t input_bytes,
                                            std::span<const std::uint32_t> block_lengths,
                                            std::vector<std::uint32_t>* block_crcs, GpuTelemetry* telemetry) {
    if (block_crcs == nullptr) {
        return compute_crc32_device(device_input, input_bytes, telemetry, "encode CRC device memory");
    }
    *block_crcs = compute_block_crc32_device(device_input, block_lengths, telemetry);
    std::uint32_t crc = 0U;
    for (std::size_t index = 0U; index < block_lengths.size(); ++index) {
        crc = crc32_combine(crc, (*block_crcs)[index], block_lengths[index]);
    }
    return crc;
}

// Purpose: Keep the previously measured dictionary/sparse winners disjoint before payload publication.
// Inputs: Equal per-block trial arrays; a nonempty dictionary candidate has already beaten its sparse competitor.
// Outputs: Releases overridden sparse storage or throws for inconsistent internal geometry.
static void discard_overridden_sparse_trials(const dictionary::DictionaryReplacements& dictionary_replacements,
                                             std::vector<std::vector<std::byte>>& sparse_replacements) {
    if (dictionary_replacements.size() != sparse_replacements.size()) {
        throw GpuError("HIP replacement portfolios have inconsistent block counts");
    }
    for (std::size_t index = 0U; index < dictionary_replacements.size(); ++index) {
        if (!dictionary_replacements[index].empty()) {
            sparse_replacements[index].clear();
        }
    }
}

// Purpose: Classify one uncompressed chunk on the AMD GPU and compute its source CRC in VRAM.
// Inputs: input is bounded host bytes, owned_input optionally owns them, and options supplies tuning.
// Optional block_lengths define exact independent boundaries; block_crcs receives their GPU checksums when supplied.
// Outputs: Returns selected GPU-native descriptors, payload bytes, and GPU source CRC; may move `owned_input` after
// success.
EncodedChunk encode_chunk_hip_impl(std::span<const std::byte> input, std::vector<std::byte>* owned_input,
                                   const GpuCodecOptions& options, std::span<const std::uint32_t> block_lengths,
                                   std::vector<std::uint32_t>* block_crcs, HipEncodeRole role) {
    if (input.empty()) {
        EncodedChunk empty;
        empty.source_crc32 = 0;
        empty.source_crc32_available = true;
        return empty;
    }
    auto* telemetry = options.telemetry.get();
    const bool source_selection = role == HipEncodeRole::Source;
    auto* phase_telemetry = source_selection ? telemetry : nullptr;
    // Nested trial work contributes device counters; its enclosing source publication owns the phase duration.
    auto phase_started = std::chrono::steady_clock::now();
    record_gpu_encode_chunk(telemetry);
    require_hip_device_ready();
    record_encode_phase(phase_telemetry, GpuEncodeStage::Readiness, phase_started);
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    const auto computed_block_count =
        block_lengths.empty() ? (input.size() + block_size - 1U) / block_size : block_lengths.size();
    if (computed_block_count > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("encode block count exceeds HIP launch limits");
    }
    const auto block_count = static_cast<std::uint32_t>(computed_block_count);
    auto host_candidates = build_encode_analysis_candidates(input, block_size, block_count, block_lengths);
    record_encode_phase(phase_telemetry, GpuEncodeStage::HostAnalysis, phase_started);
    auto classification_started = phase_started;
    HipDeviceMemoryReservation reservation(input.size(), "encode input");
    HipDeviceBuffer<std::byte> device_input(input.size(), "hipMalloc input");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(input.size()));
    record_classification_phase(phase_telemetry, GpuClassificationStage::InputAllocation, classification_started);
    {
        check_hip(copy_on_codec_stream(device_input.get(), input.data(), input.size(), hipMemcpyHostToDevice),
                  "hipMemcpy input");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(input.size()));
        record_classification_phase(phase_telemetry, GpuClassificationStage::InputUpload, classification_started);
        const auto source_crc32 =
            encoded_source_crc_hip(device_input.get(), input.size(), block_lengths, block_crcs, telemetry);
        record_classification_phase(phase_telemetry, GpuClassificationStage::SourceChecksum, classification_started);
        const auto verify_block_size =
            block_lengths.empty() ? block_size : *std::max_element(block_lengths.begin(), block_lengths.end());
        const auto mismatches = verify_encode_analysis_candidates_device(device_input.get(), input.size(),
                                                                         verify_block_size, host_candidates, telemetry);

        EncodedChunk out;
        out.source_crc32 = source_crc32;
        out.source_crc32_available = true;
        std::uint64_t encoded_offset = 0;
        std::uint64_t pattern_blocks = 0;
        const bool all_raw =
            append_verified_encode_descriptors(out, host_candidates, mismatches, encoded_offset, pattern_blocks);
        if (source_selection) {
            record_gpu_pattern_blocks(telemetry, pattern_blocks);
        }
        record_classification_phase(phase_telemetry, GpuClassificationStage::CandidateValidation,
                                    classification_started);
        record_encode_phase(phase_telemetry, GpuEncodeStage::DeviceClassification, phase_started);
        std::optional<EncodedChunk> prefix_encoded;
        if (std::ranges::any_of(out.blocks,
                                [](const BlockDescriptor& block) { return block.kind == BlockKind::Raw; })) {
            prefix_encoded = encode_native_prefix_chunk_device(device_input.get(), input, block_size, out.blocks,
                                                               options.compression_level, telemetry);
        }
        record_encode_phase(phase_telemetry, GpuEncodeStage::Prefix, phase_started);
        auto& baseline_blocks = prefix_encoded ? prefix_encoded->blocks : out.blocks;
        auto sparse_replacements =
            sparse_pattern::select_replacements(input, device_input.get(), baseline_blocks, telemetry);
        record_encode_phase(phase_telemetry, GpuEncodeStage::Sparse, phase_started);
        auto competitive_blocks = std::vector<BlockDescriptor>(baseline_blocks.begin(), baseline_blocks.end());
        // Dictionary admission compares against the smallest complete sparse/prefix candidate, not raw size alone.
        for (std::size_t index = 0U; index < sparse_replacements.size(); ++index) {
            if (!sparse_replacements[index].empty()) {
                competitive_blocks[index].kind = read_sparse_u32(sparse_replacements[index], 0U) > kMaxGpuPatternBytes
                                                     ? BlockKind::GpuLongSparsePattern
                                                     : BlockKind::GpuSparsePattern;
                competitive_blocks[index].encoded_len = static_cast<std::uint32_t>(sparse_replacements[index].size());
            }
        }
        auto dictionary_replacements = dictionary::select_dictionary_replacements(
            input, device_input.get(), competitive_blocks, options.compression_level, telemetry);
        if (options.compression_mode == NativeCompressionMode::NeutronStar) {
            dictionary::improve_neutron_replacements(input, device_input.get(), competitive_blocks,
                                                     dictionary_replacements, telemetry, options.encode_checkpoint);
        }
        record_encode_phase(phase_telemetry, GpuEncodeStage::Dictionary, phase_started);
        const bool has_dictionary = std::ranges::any_of(
            dictionary_replacements, [](const std::vector<std::byte>& replacement) { return !replacement.empty(); });
        discard_overridden_sparse_trials(dictionary_replacements, sparse_replacements);
        const bool has_sparse = std::ranges::any_of(
            sparse_replacements, [](const std::vector<std::byte>& replacement) { return !replacement.empty(); });
        device_input.reset_checked("hipFree input");
        // The original device source is released before secondary encoding; baseline bytes stay owned until selection.
        if (!prefix_encoded) {
            // Neutron trials still borrow the original bytes after publication; retain their external owner.
            auto* movable_input =
                options.compression_mode == NativeCompressionMode::NeutronStar ? nullptr : owned_input;
            append_verified_encode_payload(out, input, block_size, host_candidates, mismatches, all_raw, movable_input);
        }
        auto baseline = prefix_encoded ? std::move(*prefix_encoded) : std::move(out);
        baseline.source_crc32 = source_crc32;
        baseline.source_crc32_available = true;
        auto selected = has_dictionary
                            ? dictionary::apply_dictionary_replacements(std::move(baseline), dictionary_replacements)
                            : std::move(baseline);
        if (has_sparse) {
            selected = sparse_pattern::apply_replacements(std::move(selected), sparse_replacements);
        }
        if (options.compression_mode == NativeCompressionMode::NeutronStar) {
            selected = finish_neutron_encoding(input, std::move(selected), options, role, dictionary_replacements,
                                               sparse_replacements);
        }
        if (source_selection) {
            record_selected_block_kinds(selected, telemetry);
        }
        record_encode_phase(phase_telemetry, GpuEncodeStage::Publication, phase_started);
        return selected;
    }
}

// Purpose: Classify one borrowed uncompressed chunk through AMD HIP.
// Inputs: `input` is a bounded host chunk and `options` supplies block size plus telemetry.
// Outputs: Returns descriptors, encoded payload bytes, and GPU source CRC metadata.
EncodedChunk encode_chunk_hip(std::span<const std::byte> input, const GpuCodecOptions& options) {
    return encode_chunk_hip_impl(input, nullptr, options);
}

// Purpose: Classify one owned uncompressed chunk through AMD HIP.
// Inputs: `input` owns bytes for the duration of the call and `options` supplies block size plus telemetry.
// Outputs: Returns descriptors, payload, and GPU source CRC; moves `input` into payload only for all-raw chunks.
EncodedChunk encode_owned_chunk_hip(std::vector<std::byte>& input, const GpuCodecOptions& options) {
    return encode_chunk_hip_impl(std::span<const std::byte>(input.data(), input.size()), &input, options);
}

// Purpose: Encode a validated dense batch with independent boundaries and device-computed per-block CRCs.
// Inputs: input owns all bytes; lengths and options passed public validation before HIP dispatch.
// Outputs: Returns ordered block payloads/CRCs; input moves only after all fallible GPU cleanup succeeds.
EncodedBlockBatch encode_owned_block_batch_hip(std::vector<std::byte>& input, std::span<const std::uint32_t> lengths,
                                               const GpuCodecOptions& options) {
    EncodedBlockBatch batch;
    batch.encoded = encode_chunk_hip_impl(input, &input, options, lengths, &batch.block_crc32);
    return batch;
}

// Purpose: Decode GPU-supported block kinds into a caller-provided host buffer through AMD HIP.
// Inputs: `payload` and `blocks` are validated archive metadata, `output` is exact decoded storage, and `options`
// supplies telemetry. Outputs: Validates even empty layouts; writes decoded bytes or throws for invalid/CPU-only
// blocks.
static void decode_plain_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                   std::span<std::byte> output, const GpuCodecOptions& options) {
    if (output.empty()) {
        validate_decode_layout(payload, blocks, 0U, options.block_size);
        return;
    }
    for (const auto& block : blocks) {
        if (block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd) {
            throw GpuError("AMD HIP decode does not support CPU-compressed blocks");
        }
    }
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    require_hip_device_ready();
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, output.size(), block_size);
    auto host_blocks = build_decode_device_blocks(blocks);
    const auto prefix_plans = build_prefix_decode_segments(host_blocks);
    const auto dictionary_plans = build_dictionary_decode_segments(payload, host_blocks);

    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "decode block table");
    auto required_bytes = checked_add_bytes(payload_bytes, output.size(), "decode device memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "decode device memory");
    HipDeviceMemoryReservation reservation(required_bytes, "decode");
    HipDeviceBuffer<std::byte> device_payload(payload_bytes, "hipMalloc payload");
    HipDeviceBuffer<std::byte> device_output(output.size(), "hipMalloc output");
    HipDeviceBuffer<DeviceBlock> device_blocks(block_table_bytes, "hipMalloc decode blocks");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    if (!payload.empty()) {
        check_hip(copy_on_codec_stream(device_payload.get(), payload.data(), payload.size(), hipMemcpyHostToDevice),
                  "hipMemcpy payload");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
    }
    check_hip(copy_on_codec_stream(device_blocks.get(), host_blocks.data(), block_table_bytes, hipMemcpyHostToDevice),
              "hipMemcpy decode blocks");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));
    materialize_non_prefix_segments_device(device_payload.get(), device_blocks.get(), host_blocks, device_output.get(),
                                           output.size(), telemetry);
    materialize_sparse_patches_device(device_payload.get(), device_blocks.get(), host_blocks, device_output.get(),
                                      telemetry);
    materialize_prefix_segments_device(device_payload.get(), device_output.get(), prefix_plans, telemetry);
    dictionary::decode_segments_device(device_payload.get(), dictionary_plans, device_output.get(), telemetry);
    check_hip(copy_on_codec_stream(output.data(), device_output.get(), output.size(), hipMemcpyDeviceToHost),
              "hipMemcpy output");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(output.size()));
    device_payload.reset_checked("hipFree payload");
    device_output.reset_checked("hipFree output");
    device_blocks.reset_checked("hipFree decode blocks");
}

// Purpose: Materialize a closed two-stage Neutron block exclusively through existing HIP decoders.
// Inputs: A validated exact compound payload, its descriptor, output extent, and required device policy.
// Outputs: Decodes both stages on HIP; admits bounded intermediate RAM and never invokes a CPU materializer.
static void decode_compound_block_hip(std::span<const std::byte> encoded, const BlockDescriptor& block,
                                      std::span<std::byte> output, const GpuCodecOptions& options) {
    const auto stages = parse_gpu_compound_block(encoded, block);
    (void)resolve_host_pipeline_inflight_limit(query_host_memory_snapshot(), block.uncompressed_len);
    std::vector<std::byte> intermediate(stages.inner.uncompressed_len);
    decode_plain_chunk_hip(stages.inner_payload, std::span(&stages.inner, 1U), intermediate, options);
    decode_plain_chunk_hip(intermediate, std::span(&stages.original, 1U), output, options);
}

// Purpose: Decode a closed GPU byte-plane stage and invert its exact permutation on HIP.
// Inputs: Validated exact frame/output spans and operation-owned required-HIP policy.
// Outputs: Restores original bytes using one bounded intermediate; never invokes a CPU codec or transform.
static void decode_byte_plane_block_hip(std::span<const std::byte> encoded, const BlockDescriptor& block,
                                        std::span<std::byte> output, const GpuCodecOptions& options) {
    (void)resolve_host_pipeline_inflight_limit(query_host_memory_snapshot(), output.size());
    std::vector<std::byte> transformed(output.size());
    const auto stages = parse_gpu_byte_plane_stages(encoded, block);
    decode_plain_chunk_hip(stages.payload, std::span(stages.blocks).first(stages.count), transformed, options);
    transform_byte_planes_hip(transformed, output, stages.width, true, options);
}

// Purpose: Decode native GPU blocks, including the bounded nonrecursive version-nine composition.
// Inputs: Exact payload/layout/output spans and operation-owned HIP policy and telemetry.
// Outputs: Validates all outer and inner layouts before device work and materializes bytes through HIP only.
void decode_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                      std::span<std::byte> output, const GpuCodecOptions& options) {
    validate_decode_layout(payload, blocks, output.size(), options.block_size);
    if (std::ranges::any_of(blocks, [](const BlockDescriptor& block) {
            return block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd;
        })) {
        throw GpuError("AMD HIP decode does not support CPU-compressed blocks");
    }
    if (std::ranges::none_of(blocks, [](const BlockDescriptor& block) {
            return block.kind == BlockKind::GpuCompound || is_gpu_byte_plane_kind(block.kind);
        })) {
        decode_plain_chunk_hip(payload, blocks, output, options);
        return;
    }
    std::size_t output_offset = 0U;
    for (auto block : blocks) {
        const auto encoded = block.kind == BlockKind::Fill
                                 ? std::span<const std::byte>{}
                                 : payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
        const auto decoded = output.subspan(output_offset, block.uncompressed_len);
        block.encoded_offset = 0U;
        if (block.kind == BlockKind::GpuCompound) {
            decode_compound_block_hip(encoded, block, decoded, options);
        } else if (is_gpu_byte_plane_kind(block.kind)) {
            decode_byte_plane_block_hip(encoded, block, decoded, options);
        } else {
            decode_plain_chunk_hip(encoded, std::span(&block, 1U), decoded, options);
        }
        output_offset += block.uncompressed_len;
    }
}

// Purpose: Verify mixed composed archives with GPU decoding and GPU CRC over bounded single-block buffers.
// Inputs: A fully validated layout, output byte count and HIP telemetry configuration.
// Outputs: Returns ordered combined source CRC without any CPU codec or checksum substitution.
static std::uint32_t crc_compound_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                            const GpuCodecOptions& options) {
    std::uint32_t crc = 0U;
    for (auto block : blocks) {
        (void)resolve_host_pipeline_inflight_limit(query_host_memory_snapshot(), block.uncompressed_len);
        std::vector<std::byte> decoded(block.uncompressed_len);
        const auto encoded = block.kind == BlockKind::Fill
                                 ? std::span<const std::byte>{}
                                 : payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
        block.encoded_offset = 0U;
        decode_chunk_hip(encoded, std::span(&block, 1U), decoded, options);
        HipDeviceMemoryReservation reservation(decoded.size(), "compound CRC input");
        HipDeviceBuffer<std::byte> device_input(decoded.size(), "hipMalloc compound CRC input");
        auto* telemetry = options.telemetry.get();
        record_gpu_device_allocation_bytes(telemetry, decoded.size());
        check_hip(copy_on_codec_stream(device_input.get(), decoded.data(), decoded.size(), hipMemcpyHostToDevice),
                  "hipMemcpy compound CRC input");
        record_gpu_h2d_bytes(telemetry, decoded.size());
        const auto block_crc =
            compute_crc32_device(device_input.get(), decoded.size(), telemetry, "compound CRC workspace");
        crc = crc32_combine(crc, block_crc, block.uncompressed_len);
        device_input.reset_checked("hipFree compound CRC input");
    }
    return crc;
}

// Purpose: Verify a decoded chunk by checksumming GPU-supported block metadata directly in VRAM.
// Inputs: `payload`/`blocks` describe encoded bytes, `output_size` is decoded byte count, and `options` supplies
// telemetry. Outputs: Validates even empty layouts, then returns CRC while copying back only compact segment metadata.
std::uint32_t crc_decoded_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                    std::uint64_t output_size, const GpuCodecOptions& options) {
    if (output_size == 0) {
        validate_decode_layout(payload, blocks, 0U, options.block_size);
        return 0;
    }
    if (output_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("decode output exceeds SuperZip resource limit");
    }
    for (const auto& block : blocks) {
        if (block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd) {
            throw GpuError("AMD HIP CRC verification does not support CPU-compressed blocks");
        }
    }
    auto* telemetry = options.telemetry.get();
    require_hip_device_ready();
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, static_cast<std::size_t>(output_size), block_size);

    if (std::ranges::any_of(blocks, [](const BlockDescriptor& block) {
            return block.kind == BlockKind::GpuCompound || is_gpu_byte_plane_kind(block.kind);
        })) {
        return crc_compound_chunk_hip(payload, blocks, options);
    }

    bool needs_materialized_crc = false;
    for (const auto& block : blocks) {
        needs_materialized_crc |= is_gpu_prefix_block(block) || block.kind == BlockKind::GpuDictionary ||
                                  is_gpu_sparse_pattern_kind(block.kind);
    }
    if (needs_materialized_crc) {
        return compute_materialized_crc32_device(payload, blocks, output_size, options);
    }

    record_gpu_decode_chunk(telemetry);

    if (decoded_crc_uses_contiguous_raw_payload(payload, blocks, output_size)) {
        const auto payload_bytes = static_cast<std::size_t>(output_size);
        HipDeviceMemoryReservation reservation(payload_bytes, "CRC raw payload");
        HipDeviceBuffer<std::byte> device_payload(payload_bytes, "hipMalloc CRC raw payload");
        record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(payload_bytes));
        check_hip(copy_on_codec_stream(device_payload.get(), payload.data(), payload_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy CRC raw payload");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload_bytes));
        const auto crc =
            compute_crc32_device(device_payload.get(), output_size, telemetry, "decoded raw CRC device memory");
        device_payload.reset_checked("hipFree CRC raw payload");
        return crc;
    }

    auto host_blocks = build_decode_device_blocks(blocks);

    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "CRC decode block table");
    const auto required_bytes = checked_add_bytes(payload_bytes, block_table_bytes, "CRC decode device memory");
    HipDeviceMemoryReservation reservation(required_bytes, "CRC decode");
    HipDeviceBuffer<std::byte> device_payload(payload_bytes, "hipMalloc CRC payload");
    HipDeviceBuffer<DeviceBlock> device_blocks(block_table_bytes, "hipMalloc CRC decode blocks");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    if (!payload.empty()) {
        check_hip(copy_on_codec_stream(device_payload.get(), payload.data(), payload.size(), hipMemcpyHostToDevice),
                  "hipMemcpy CRC payload");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
    }
    check_hip(copy_on_codec_stream(device_blocks.get(), host_blocks.data(), block_table_bytes, hipMemcpyHostToDevice),
              "hipMemcpy CRC decode blocks");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));

    const auto crc = compute_decoded_crc32_device(device_payload.get(), device_blocks.get(),
                                                  static_cast<std::uint32_t>(host_blocks.size()), output_size,
                                                  telemetry, "decoded CRC device memory");
    device_payload.reset_checked("hipFree CRC payload");
    device_blocks.reset_checked("hipFree CRC decode blocks");
    return crc;
}

}  // namespace superzip
