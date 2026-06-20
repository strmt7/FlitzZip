#include "gpu/gpu_codec.hpp"
#include "gpu/hip_codec_support.hpp"

#include "core/checksum.hpp"
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
GpuInfo query_hip_gpu_info();

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
                                                  std::uint32_t block_size, const DeviceBlock* candidates,
                                                  std::uint32_t* mismatches, std::uint32_t block_count,
                                                  std::uint32_t segments_per_block);

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

// Purpose: Read one bit from a GPU prefix segment.
// Inputs: `stream` is a byte-aligned encoded segment and `bit_pos` is advanced in place.
// Outputs: Returns the bit, or zero if malformed metadata points past the encoded segment.
__device__ std::uint32_t gpu_prefix_read_bit(const std::byte* stream, std::uint32_t& bit_pos,
                                             std::uint32_t limit_bits) {
    if (bit_pos >= limit_bits) {
        return 0U;
    }
    const auto byte_index = bit_pos / 8U;
    const auto bit_index = bit_pos % 8U;
    ++bit_pos;
    return (static_cast<std::uint8_t>(stream[byte_index]) >> bit_index) & 1U;
}

// Purpose: Read a small little-bit-order field from a GPU prefix segment.
// Inputs: `stream`, `bit_pos`, `limit_bits`, and `width` describe the encoded field.
// Outputs: Returns the decoded field; malformed short segments decode as zero-padded.
__device__ std::uint32_t gpu_prefix_read_bits(const std::byte* stream, std::uint32_t& bit_pos, std::uint32_t limit_bits,
                                              std::uint32_t width) {
    std::uint32_t value = 0;
    for (std::uint32_t bit = 0; bit < width; ++bit) {
        value |= gpu_prefix_read_bit(stream, bit_pos, limit_bits) << bit;
    }
    return value;
}

// Purpose: Decode one byte from the static GPU prefix code.
// Inputs: `stream`, `bit_pos`, and `limit_bits` describe one encoded segment.
// Outputs: Returns the decoded byte; invalid high-byte payloads clamp to zero.
__device__ std::byte gpu_prefix_decode_byte(const std::byte* stream, std::uint32_t& bit_pos, std::uint32_t limit_bits) {
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(gpu_prefix_read_bits(stream, bit_pos, limit_bits, 2U));
    }
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(4U + gpu_prefix_read_bits(stream, bit_pos, limit_bits, 4U));
    }
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(20U + gpu_prefix_read_bits(stream, bit_pos, limit_bits, 6U));
    }
    const auto high = gpu_prefix_read_bits(stream, bit_pos, limit_bits, 8U);
    return high <= 171U ? static_cast<std::byte>(84U + high) : std::byte{0};
}

// Purpose: Decode one byte from an adaptive GPU prefix segment.
// Inputs: `codebook`, `stream`, `bit_pos`, and `limit_bits` describe the encoded byte stream.
// Outputs: Returns the decoded byte, using literal high-group bytes when the value was not in the codebook.
__device__ std::byte gpu_adaptive_prefix_decode_byte(const std::byte* codebook, const std::byte* stream,
                                                     std::uint32_t& bit_pos, std::uint32_t limit_bits) {
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[gpu_prefix_read_bits(stream, bit_pos, limit_bits, 2U)];
    }
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[kGpuAdaptivePrefixSmallSymbols + gpu_prefix_read_bits(stream, bit_pos, limit_bits, 4U)];
    }
    if (gpu_prefix_read_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols +
                        gpu_prefix_read_bits(stream, bit_pos, limit_bits, 6U)];
    }
    return static_cast<std::byte>(gpu_prefix_read_bits(stream, bit_pos, limit_bits, 8U));
}

// Purpose: Decode GPU prefix segments into the final device output buffer.
// Inputs: `payload` is the encoded archive payload, `plans` describes each prefix segment, and `output` is decoded
// storage. Outputs: Writes decoded bytes for every prefix segment.
__global__ void materialize_prefix_segments_kernel(const std::byte* payload, const PrefixDecodeSegment* plans,
                                                   std::uint32_t plan_count, std::byte* output) {
    const auto plan_index = static_cast<std::uint32_t>(blockIdx.x);
    if (plan_index >= plan_count) {
        return;
    }
    const auto& plan = plans[plan_index];
    const auto* table = payload + plan.table_offset;
    const auto encoded_start = gpu_prefix_read_u32(table + (static_cast<std::size_t>(plan.segment_index) * 4U));
    const auto encoded_end = gpu_prefix_read_u32(table + ((static_cast<std::size_t>(plan.segment_index) + 1U) * 4U));
    if (encoded_end < encoded_start) {
        return;
    }
    const auto* stream = payload + plan.bitstream_offset + encoded_start;
    const auto limit_bits = (encoded_end - encoded_start) * 8U;
    const auto* codebook = payload + plan.codebook_offset;
    std::uint32_t bit_pos = 0;
    for (std::uint32_t i = 0; i < plan.decoded_len; ++i) {
        output[plan.output_offset + i] = plan.adaptive != 0U
                                             ? gpu_adaptive_prefix_decode_byte(codebook, stream, bit_pos, limit_bits)
                                             : gpu_prefix_decode_byte(stream, bit_pos, limit_bits);
    }
}

// Purpose: Return the device memory needed to verify provisional encode candidates.
// Inputs: `block_count` is the number of archive blocks and `needs_verification` selects the active path.
// Outputs: Returns zero when all candidates are raw, otherwise candidate plus mismatch table bytes.
std::size_t encode_analysis_device_bytes(std::uint32_t block_count, bool needs_verification) {
    if (!needs_verification) {
        return 0;
    }
    const auto candidate_table_bytes = checked_multiply_bytes(block_count, sizeof(DeviceBlock), "encode candidates");
    const auto mismatch_table_bytes = checked_multiply_bytes(block_count, sizeof(std::uint32_t), "encode mismatches");
    return checked_add_bytes(candidate_table_bytes, mismatch_table_bytes, "encode analysis memory");
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
    DeviceBlock* device_candidates = nullptr;
    std::uint32_t* device_mismatches = nullptr;
    check_hip(hipMalloc(&device_candidates, candidate_table_bytes), "hipMalloc encode candidates");
    check_hip(hipMalloc(&device_mismatches, mismatch_table_bytes), "hipMalloc encode mismatches");
    try {
        check_hip(hipMemcpy(device_candidates, candidates.data(), candidate_table_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy encode candidates");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_table_bytes));
        check_hip(hipMemset(device_mismatches, 0, mismatch_table_bytes), "hipMemset encode mismatches");
        const auto segments_per_block = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(block_size) + kAnalyzeSegmentBytes - 1U) / kAnalyzeSegmentBytes);
        const auto grid64 = static_cast<std::uint64_t>(segments_per_block) * block_count;
        if (grid64 > std::numeric_limits<unsigned int>::max()) {
            throw GpuError("encode candidate segment count exceeds HIP launch limits");
        }
        auto events = make_hip_event_pair("create verify_analysis_candidates_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record verify_analysis_candidates_kernel start");
        verify_analysis_candidates_kernel<<<static_cast<unsigned int>(grid64), 256>>>(
            device_input, input_len, block_size, device_candidates, device_mismatches, block_count, segments_per_block);
        check_hip(hipGetLastError(), "launch verify_analysis_candidates_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record verify_analysis_candidates_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize verify_analysis_candidates_kernel");
        check_hip(hipMemcpy(mismatches.data(), device_mismatches, mismatch_table_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy encode mismatches");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(mismatch_table_bytes));
        check_hip(hipFree(device_candidates), "hipFree encode candidates");
        check_hip(hipFree(device_mismatches), "hipFree encode mismatches");
        return mismatches;
    } catch (...) {
        (void)hipFree(device_candidates);
        (void)hipFree(device_mismatches);
        throw;
    }
}

// Purpose: Choose the smallest GPU-native all-raw replacement allowed by the compression level.
// Inputs: `device_input`, `input`, block settings, `compression_level`, and `telemetry` describe one uploaded chunk.
// Outputs: Returns the best GPU-native encoded chunk, or empty when raw storage is smallest.
std::optional<EncodedChunk> encode_all_raw_native_chunk_device(const std::byte* device_input,
                                                               std::span<const std::byte> input,
                                                               std::uint32_t block_size, std::uint32_t block_count,
                                                               int compression_level, GpuTelemetry* telemetry) {
    auto fixed = encode_all_raw_prefix_chunk_device(device_input, input, block_size, block_count, telemetry);
    if (compression_level < 7) {
        if (fixed) {
            record_gpu_prefix_blocks(telemetry, count_emitted_prefix_blocks(*fixed));
        }
        return fixed;
    }
    auto adaptive = encode_all_raw_adaptive_prefix_chunk_device(device_input, input, block_size, block_count,
                                                                compression_level, telemetry);
    if (!adaptive) {
        if (fixed) {
            record_gpu_prefix_blocks(telemetry, count_emitted_prefix_blocks(*fixed));
        }
        return fixed;
    }
    if (!fixed || adaptive->payload.size() < fixed->payload.size()) {
        record_gpu_prefix_blocks(telemetry, count_emitted_prefix_blocks(*adaptive));
        return adaptive;
    }
    record_gpu_prefix_blocks(telemetry, count_emitted_prefix_blocks(*fixed));
    return fixed;
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

// Purpose: Append compact verified GPU payload bytes for non-fill SUZIP blocks.
// Inputs: `input`, `block_size`, `candidates`, and `mismatches` describe verified encode output.
// Outputs: Mutates `out.payload` with raw or compact pattern bytes in descriptor order.
void append_verified_encode_payload(EncodedChunk& out, std::span<const std::byte> input, std::uint32_t block_size,
                                    std::span<const DeviceBlock> candidates,
                                    std::span<const std::uint32_t> mismatches) {
    out.payload.reserve(input.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto effective_kind = verified_candidate_kind(candidates[i], mismatches[i]);
        if (effective_kind == static_cast<std::uint8_t>(BlockKind::Fill)) {
            continue;
        }
        const auto start = i * static_cast<std::size_t>(block_size);
        const auto len = static_cast<std::size_t>(candidates[i].uncompressed_len);
        const auto encoded_len = effective_kind == static_cast<std::uint8_t>(BlockKind::Pattern)
                                     ? static_cast<std::size_t>(candidates[i].encoded_len)
                                     : len;
        out.payload.insert(out.payload.end(), input.begin() + start, input.begin() + start + encoded_len);
    }
}

// Purpose: Verify provisional fill/pattern encode candidates over fixed-size GPU tiles.
// Inputs: `input`, `block_size`, `candidates`, and `segments_per_block` describe candidate work; `mismatches` is one
// flag per block. Outputs: Atomically marks blocks whose sampled candidate does not describe the full block.
__global__ void verify_analysis_candidates_kernel(const std::byte* input, std::size_t input_len,
                                                  std::uint32_t block_size, const DeviceBlock* candidates,
                                                  std::uint32_t* mismatches, std::uint32_t block_count,
                                                  std::uint32_t segments_per_block) {
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
    const auto block_start = static_cast<std::size_t>(block_index) * block_size;
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
// Outputs: Writes decoded bytes into `output`; invalid metadata leaves affected bytes untouched instead of OOB access.
__global__ void materialize_segments_kernel(const std::byte* payload, const DeviceBlock* blocks,
                                            std::uint32_t block_count, std::byte* output, std::size_t output_len) {
    const auto segment_start = static_cast<std::size_t>(blockIdx.x) * kMaterializeSegmentBytes;
    if (segment_start >= output_len) {
        return;
    }
    const auto segment_end = min(segment_start + static_cast<std::size_t>(kMaterializeSegmentBytes), output_len);
    const auto first_block = find_decoded_block(blocks, block_count, segment_start);
    for (auto pos = segment_start + threadIdx.x; pos < segment_end; pos += blockDim.x) {
        auto block_index = first_block;
        while (block_index < block_count) {
            const auto block_start = static_cast<std::size_t>(blocks[block_index].output_offset);
            const auto block_end = block_start + static_cast<std::size_t>(blocks[block_index].uncompressed_len);
            if (pos >= block_start && pos < block_end) {
                break;
            }
            ++block_index;
        }
        if (block_index >= block_count) {
            continue;
        }
        const auto& block = blocks[block_index];
        const auto in_block = pos - static_cast<std::size_t>(block.output_offset);
        if (block.kind == 1) {
            output[pos] = static_cast<std::byte>(block.fill_value);
        } else if (block.kind == 0) {
            output[pos] = payload[block.encoded_offset + in_block];
        } else if (block.kind == 3 && block.encoded_len != 0U) {
            output[pos] = payload[block.encoded_offset + (in_block % block.encoded_len)];
        }
    }
}

// Purpose: Compute one finalized CRC-32 value per fixed-size segment of a device buffer.
// Inputs: `input`/`input_len` describe device bytes, `segments` is a device output table, and `segment_count` bounds
// the launch. Outputs: Writes ordered segment CRCs and segment lengths for host-side GF(2) concatenation.
__global__ void crc32_segments_kernel(const std::byte* input, std::size_t input_len, DeviceCrcSegment* segments,
                                      std::uint32_t segment_count) {
    const auto segment_index = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (segment_index >= segment_count) {
        return;
    }
    const auto start = static_cast<std::size_t>(segment_index) * kCrcSegmentBytes;
    const auto end = min(start + static_cast<std::size_t>(kCrcSegmentBytes), input_len);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t pos = start; pos < end; ++pos) {
        const auto octet = static_cast<std::uint8_t>(input[pos]);
        crc = kDeviceCrc32Table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
    }
    segments[segment_index] = DeviceCrcSegment{
        .crc32 = crc ^ 0xFFFFFFFFU,
        .length = static_cast<std::uint32_t>(end - start),
    };
}

// Purpose: Locate the decoded block that contains one output byte offset.
// Inputs: `blocks`/`block_count` describe a validated decoded layout and `output_offset` is a byte position.
// Outputs: Returns a block index less than `block_count`, or `block_count` if metadata is inconsistent.
__device__ std::uint32_t find_decoded_block(const DeviceBlock* blocks, std::uint32_t block_count,
                                            std::size_t output_offset) {
    for (std::uint32_t index = 0; index < block_count; ++index) {
        const auto start = static_cast<std::size_t>(blocks[index].output_offset);
        const auto length = static_cast<std::size_t>(blocks[index].uncompressed_len);
        if (output_offset >= start && output_offset < start + length) {
            return index;
        }
    }
    return block_count;
}

// Purpose: Compute finalized CRC-32 values for decoded-stream segments without a decoded temporary buffer.
// Inputs: `payload`/`blocks` describe HIP-supported encoded data, `output_len` bounds decoded bytes, and `segments`
// receives one CRC result per fixed-size decoded segment.
// Outputs: Writes ordered segment CRCs and lengths for host-side CRC concatenation.
__global__ void decoded_crc32_segments_kernel(const std::byte* payload, const DeviceBlock* blocks,
                                              std::uint32_t block_count, std::size_t output_len,
                                              DeviceCrcSegment* segments, std::uint32_t segment_count) {
    const auto segment_index = static_cast<std::uint32_t>(blockIdx.x * blockDim.x + threadIdx.x);
    if (segment_index >= segment_count) {
        return;
    }
    const auto start = static_cast<std::size_t>(segment_index) * kCrcSegmentBytes;
    const auto end = min(start + static_cast<std::size_t>(kCrcSegmentBytes), output_len);
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
    segments[segment_index] = DeviceCrcSegment{
        .crc32 = crc ^ 0xFFFFFFFFU,
        .length = static_cast<std::uint32_t>(end - start),
    };
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

// Purpose: Return the number of fixed-size CRC segments needed for a device buffer.
// Inputs: `bytes` is the buffer length to checksum.
// Outputs: Returns zero for empty buffers or a bounded segment count for nonempty buffers.
std::uint32_t crc_segment_count(std::uint64_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    const auto segments = (bytes + kCrcSegmentBytes - 1U) / kCrcSegmentBytes;
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
    const auto segments = crc_segment_count(input_len);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    DeviceCrcSegment* device_segments = nullptr;
    ensure_device_memory_budget(segment_bytes, action);
    check_hip(hipMalloc(&device_segments, segment_bytes), "hipMalloc CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    try {
        constexpr int threads = 256;
        const auto grid = static_cast<unsigned int>((segments + threads - 1U) / threads);
        auto events = make_hip_event_pair("create crc32_segments_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record crc32_segments_kernel start");
        crc32_segments_kernel<<<grid, threads>>>(device_input, static_cast<std::size_t>(input_len), device_segments,
                                                 segments);
        check_hip(hipGetLastError(), "launch crc32_segments_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record crc32_segments_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize crc32_segments_kernel");

        std::vector<DeviceCrcSegment> host_segments(segments);
        check_hip(hipMemcpy(host_segments.data(), device_segments, segment_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy CRC segments");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
        check_hip(hipFree(device_segments), "hipFree CRC segments");
        return combine_crc_segments(host_segments);
    } catch (...) {
        (void)hipFree(device_segments);
        throw;
    }
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
    const auto segments = crc_segment_count(output_len);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    DeviceCrcSegment* device_segments = nullptr;
    ensure_device_memory_budget(segment_bytes, action);
    check_hip(hipMalloc(&device_segments, segment_bytes), "hipMalloc decoded CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    try {
        constexpr int threads = 256;
        const auto grid = static_cast<unsigned int>((segments + threads - 1U) / threads);
        auto events = make_hip_event_pair("create decoded_crc32_segments_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record decoded_crc32_segments_kernel start");
        decoded_crc32_segments_kernel<<<grid, threads>>>(device_payload, device_blocks, block_count,
                                                         static_cast<std::size_t>(output_len), device_segments,
                                                         segments);
        check_hip(hipGetLastError(), "launch decoded_crc32_segments_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record decoded_crc32_segments_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize decoded_crc32_segments_kernel");

        std::vector<DeviceCrcSegment> host_segments(segments);
        check_hip(hipMemcpy(host_segments.data(), device_segments, segment_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy decoded CRC segments");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
        check_hip(hipFree(device_segments), "hipFree decoded CRC segments");
        return combine_crc_segments(host_segments);
    } catch (...) {
        (void)hipFree(device_segments);
        throw;
    }
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

// Purpose: Build GPU decode plans for every prefix-coded segment in one decoded chunk.
// Inputs: `host_blocks` is the validated device-block table with decoded output offsets.
// Outputs: Returns one decode plan per 4 KiB prefix segment.
std::vector<PrefixDecodeSegment> build_prefix_decode_segments(std::span<const DeviceBlock> host_blocks) {
    std::vector<PrefixDecodeSegment> plans;
    for (const auto& block : host_blocks) {
        const bool adaptive = block.kind == static_cast<std::uint8_t>(BlockKind::GpuAdaptivePrefix);
        if (block.kind != static_cast<std::uint8_t>(BlockKind::GpuPrefix) && !adaptive) {
            continue;
        }
        const auto segment_count = (block.uncompressed_len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
        const auto table_offset =
            block.encoded_offset + (adaptive ? static_cast<std::uint64_t>(kGpuAdaptivePrefixCodebookBytes) : 0U);
        const auto table_bytes = static_cast<std::uint64_t>(segment_count + 1U) * sizeof(std::uint32_t);
        plans.reserve(plans.size() + segment_count);
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
                .adaptive = adaptive ? 1U : 0U,
            });
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
    check_hip(hipEventRecord(events.start, nullptr), "record materialize_segments_kernel start");
    materialize_segments_kernel<<<static_cast<unsigned int>(segments), threads>>>(
        device_payload, device_blocks, static_cast<std::uint32_t>(host_blocks.size()), device_output, output_len);
    check_hip(hipGetLastError(), "launch materialize_segments_kernel");
    check_hip(hipEventRecord(events.stop, nullptr), "record materialize_segments_kernel stop");
    finish_measured_kernel(telemetry, events, "synchronize materialize_segments_kernel");
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
    PrefixDecodeSegment* device_plans = nullptr;
    check_hip(hipMalloc(&device_plans, plan_bytes), "hipMalloc prefix decode plans");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes));
    try {
        check_hip(hipMemcpy(device_plans, plans.data(), plan_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy prefix decode plans");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes));
        auto events = make_hip_event_pair("create materialize_prefix_segments_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record materialize_prefix_segments_kernel start");
        materialize_prefix_segments_kernel<<<static_cast<unsigned int>(plans.size()), 1>>>(
            device_payload, device_plans, static_cast<std::uint32_t>(plans.size()), device_output);
        check_hip(hipGetLastError(), "launch materialize_prefix_segments_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record materialize_prefix_segments_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize materialize_prefix_segments_kernel");
        check_hip(hipFree(device_plans), "hipFree prefix decode plans");
        device_plans = nullptr;
    } catch (...) {
        (void)hipFree(device_plans);
        throw;
    }
}

// Purpose: Verify decoded bytes for prefix-capable chunks without copying the decoded chunk back to the host.
// Inputs: `payload`, `blocks`, `output_size`, and `options` describe one validated decoded chunk.
// Outputs: Returns a finalized CRC-32 computed from a GPU-materialized output buffer.
std::uint32_t compute_prefix_capable_crc32_device(std::span<const std::byte> payload,
                                                  std::span<const BlockDescriptor> blocks, std::uint64_t output_size,
                                                  const GpuCodecOptions& options) {
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    auto host_blocks = build_decode_device_blocks(blocks);
    const auto prefix_plans = build_prefix_decode_segments(host_blocks);
    std::byte* device_payload = nullptr;
    std::byte* device_output = nullptr;
    DeviceBlock* device_blocks = nullptr;
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "prefix CRC decode block table");
    auto required_bytes =
        checked_add_bytes(payload_bytes, static_cast<std::size_t>(output_size), "prefix CRC decode memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "prefix CRC decode memory");
    ensure_device_memory_budget(required_bytes, "prefix CRC decode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc prefix CRC payload");
    check_hip(hipMalloc(&device_output, static_cast<std::size_t>(output_size)), "hipMalloc prefix CRC output");
    check_hip(hipMalloc(&device_blocks, block_table_bytes), "hipMalloc prefix CRC blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice),
                      "hipMemcpy prefix CRC payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
        }
        check_hip(hipMemcpy(device_blocks, host_blocks.data(), block_table_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy prefix CRC blocks");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));
        materialize_non_prefix_segments_device(device_payload, device_blocks, host_blocks, device_output,
                                               static_cast<std::size_t>(output_size), telemetry);
        materialize_prefix_segments_device(device_payload, device_output, prefix_plans, telemetry);
        const auto crc = compute_crc32_device(device_output, output_size, telemetry, "prefix decoded CRC");
        check_hip(hipFree(device_payload), "hipFree prefix CRC payload");
        device_payload = nullptr;
        check_hip(hipFree(device_output), "hipFree prefix CRC output");
        device_output = nullptr;
        check_hip(hipFree(device_blocks), "hipFree prefix CRC blocks");
        return crc;
    } catch (...) {
        (void)hipFree(device_payload);
        (void)hipFree(device_output);
        (void)hipFree(device_blocks);
        throw;
    }
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
    ensure_device_memory_budget(required_bytes, "diagnostic");

    std::vector<std::uint32_t> host(words);
    for (std::size_t i = 0; i < host.size(); ++i) {
        host[i] = static_cast<std::uint32_t>(i * 2654435761U);
    }

    std::uint32_t* device_data = nullptr;
    unsigned long long* device_partials = nullptr;
    check_hip(hipMalloc(&device_data, static_cast<std::size_t>(bytes)), "hipMalloc diagnostic data");
    check_hip(hipMalloc(&device_partials, partial_bytes), "hipMalloc diagnostic partials");
    try {
        check_hip(hipMemcpy(device_data, host.data(), static_cast<std::size_t>(bytes), hipMemcpyHostToDevice),
                  "hipMemcpy diagnostic input");

        GpuDiagnosticResult result;
        result.info = info;
        result.bytes = bytes;
        result.h2d_bytes = bytes;
        result.device_allocation_bytes = static_cast<std::uint64_t>(required_bytes);

        const auto started = std::chrono::steady_clock::now();
        std::uint32_t seed = 0xA5A5A5A5U;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < options.seconds) {
            auto events = make_hip_event_pair("create diagnostic_compute_kernel events");
            check_hip(hipEventRecord(events.start, nullptr), "record diagnostic_compute_kernel start");
            diagnostic_compute_kernel<<<blocks, threads>>>(device_data, words, seed, options.inner_iterations);
            check_hip(hipGetLastError(), "launch diagnostic_compute_kernel");
            check_hip(hipEventRecord(events.stop, nullptr), "record diagnostic_compute_kernel stop");
            check_hip(hipEventSynchronize(events.stop), "synchronize diagnostic_compute_kernel");
            float milliseconds = 0.0F;
            check_hip(hipEventElapsedTime(&milliseconds, events.start, events.stop), "time diagnostic_compute_kernel");
            result.kernel_ms += static_cast<double>(milliseconds);
            ++result.kernel_launches;
            seed += 0x9E3779B9U;
        }

        auto checksum_events = make_hip_event_pair("create diagnostic_checksum_kernel events");
        check_hip(hipEventRecord(checksum_events.start, nullptr), "record diagnostic_checksum_kernel start");
        diagnostic_checksum_kernel<<<blocks, threads, threads * sizeof(unsigned long long)>>>(device_data, words,
                                                                                              device_partials);
        check_hip(hipGetLastError(), "launch diagnostic_checksum_kernel");
        check_hip(hipEventRecord(checksum_events.stop, nullptr), "record diagnostic_checksum_kernel stop");
        check_hip(hipEventSynchronize(checksum_events.stop), "synchronize diagnostic_checksum_kernel");
        float checksum_ms = 0.0F;
        check_hip(hipEventElapsedTime(&checksum_ms, checksum_events.start, checksum_events.stop),
                  "time diagnostic_checksum_kernel");
        result.kernel_ms += static_cast<double>(checksum_ms);
        ++result.kernel_launches;

        std::vector<unsigned long long> partials(blocks);
        check_hip(hipMemcpy(partials.data(), device_partials, partial_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy diagnostic partials");
        result.d2h_bytes = static_cast<std::uint64_t>(partial_bytes);
        result.checksum = std::accumulate(partials.begin(), partials.end(), 0ULL);
        result.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        check_hip(hipFree(device_data), "hipFree diagnostic data");
        check_hip(hipFree(device_partials), "hipFree diagnostic partials");
        return result;
    } catch (...) {
        (void)hipFree(device_data);
        (void)hipFree(device_partials);
        throw;
    }
}

// Purpose: Classify one uncompressed chunk on the AMD GPU and compute its source CRC in VRAM.
// Inputs: `input` is a bounded host chunk, `owned_input` optionally owns the same bytes, and `options` supplies tuning.
// Outputs: Returns fill/raw/pattern descriptors, payload bytes, and GPU source CRC; may move `owned_input` after
// success.
EncodedChunk encode_chunk_hip_impl(std::span<const std::byte> input, std::vector<std::byte>* owned_input,
                                   const GpuCodecOptions& options) {
    if (input.empty()) {
        EncodedChunk empty;
        empty.source_crc32 = 0;
        empty.source_crc32_available = true;
        return empty;
    }
    auto* telemetry = options.telemetry.get();
    record_gpu_encode_chunk(telemetry);
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    const auto computed_block_count = (input.size() + block_size - 1U) / block_size;
    if (computed_block_count > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("encode block count exceeds HIP launch limits");
    }
    const auto block_count = static_cast<std::uint32_t>(computed_block_count);
    auto host_candidates = build_encode_analysis_candidates(input, block_size, block_count);
    const bool needs_candidate_verification = has_non_raw_analysis_candidate(host_candidates);
    const auto analysis_bytes = encode_analysis_device_bytes(block_count, needs_candidate_verification);
    const auto required_bytes = checked_add_bytes(input.size(), analysis_bytes, "encode device memory");
    ensure_device_memory_budget(required_bytes, "encode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));

    std::byte* device_input = nullptr;
    check_hip(hipMalloc(&device_input, input.size()), "hipMalloc input");
    try {
        check_hip(hipMemcpy(device_input, input.data(), input.size(), hipMemcpyHostToDevice), "hipMemcpy input");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(input.size()));
        const auto source_crc32 = compute_crc32_device(device_input, static_cast<std::uint64_t>(input.size()),
                                                       telemetry, "encode CRC device memory");
        const auto mismatches = verify_encode_analysis_candidates_device(device_input, input.size(), block_size,
                                                                         host_candidates, telemetry);

        EncodedChunk out;
        out.source_crc32 = source_crc32;
        out.source_crc32_available = true;
        std::uint64_t encoded_offset = 0;
        std::uint64_t pattern_blocks = 0;
        const bool all_raw =
            append_verified_encode_descriptors(out, host_candidates, mismatches, encoded_offset, pattern_blocks);
        record_gpu_pattern_blocks(telemetry, pattern_blocks);
        if (all_raw) {
            if (auto prefix_encoded = encode_all_raw_native_chunk_device(device_input, input, block_size, block_count,
                                                                         options.compression_level, telemetry)) {
                prefix_encoded->source_crc32 = source_crc32;
                prefix_encoded->source_crc32_available = true;
                check_hip(hipFree(device_input), "hipFree input");
                return std::move(*prefix_encoded);
            }
            if (owned_input != nullptr) {
                out.payload = std::move(*owned_input);
            } else {
                out.payload.resize(input.size());
                std::copy(input.begin(), input.end(), out.payload.begin());
            }
        } else {
            append_verified_encode_payload(out, input, block_size, host_candidates, mismatches);
        }
        check_hip(hipFree(device_input), "hipFree input");
        return out;
    } catch (...) {
        (void)hipFree(device_input);
        throw;
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

// Purpose: Decode GPU-supported block kinds into a caller-provided host buffer through AMD HIP.
// Inputs: `payload` and `blocks` are validated archive metadata, `output` is exact decoded storage, and `options`
// supplies telemetry. Outputs: Writes decoded bytes into `output`; throws `GpuError` when CPU-deflate blocks require
// the CPU codec.
void decode_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                      std::span<std::byte> output, const GpuCodecOptions& options) {
    if (output.empty()) {
        return;
    }
    for (const auto& block : blocks) {
        if (block.kind == BlockKind::Deflate) {
            throw GpuError("AMD HIP decode does not support CPU-deflate blocks");
        }
    }
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, output.size(), block_size);
    auto host_blocks = build_decode_device_blocks(blocks);
    const auto prefix_plans = build_prefix_decode_segments(host_blocks);

    std::byte* device_payload = nullptr;
    std::byte* device_output = nullptr;
    DeviceBlock* device_blocks = nullptr;
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "decode block table");
    auto required_bytes = checked_add_bytes(payload_bytes, output.size(), "decode device memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "decode device memory");
    ensure_device_memory_budget(required_bytes, "decode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc payload");
    check_hip(hipMalloc(&device_output, output.size()), "hipMalloc output");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * host_blocks.size()), "hipMalloc decode blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice),
                      "hipMemcpy payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
        }
        check_hip(hipMemcpy(device_blocks, host_blocks.data(), sizeof(DeviceBlock) * host_blocks.size(),
                            hipMemcpyHostToDevice),
                  "hipMemcpy decode blocks");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));
        materialize_non_prefix_segments_device(device_payload, device_blocks, host_blocks, device_output, output.size(),
                                               telemetry);
        materialize_prefix_segments_device(device_payload, device_output, prefix_plans, telemetry);
        check_hip(hipMemcpy(output.data(), device_output, output.size(), hipMemcpyDeviceToHost), "hipMemcpy output");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(output.size()));
        check_hip(hipFree(device_payload), "hipFree payload");
        check_hip(hipFree(device_output), "hipFree output");
        check_hip(hipFree(device_blocks), "hipFree decode blocks");
    } catch (...) {
        (void)hipFree(device_payload);
        (void)hipFree(device_output);
        (void)hipFree(device_blocks);
        throw;
    }
}

// Purpose: Verify a decoded chunk by checksumming GPU-supported block metadata directly in VRAM.
// Inputs: `payload`/`blocks` describe encoded bytes, `output_size` is decoded byte count, and `options` supplies
// telemetry. Outputs: Returns ZIP-compatible CRC-32 while copying back only compact CRC segment metadata.
std::uint32_t crc_decoded_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                    std::uint64_t output_size, const GpuCodecOptions& options) {
    if (output_size == 0) {
        return 0;
    }
    if (output_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("decode output exceeds SuperZip resource limit");
    }
    for (const auto& block : blocks) {
        if (block.kind == BlockKind::Deflate) {
            throw GpuError("AMD HIP CRC verification does not support CPU-deflate blocks");
        }
    }
    auto* telemetry = options.telemetry.get();
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, static_cast<std::size_t>(output_size), block_size);

    if (std::ranges::any_of(blocks, is_gpu_prefix_block)) {
        return compute_prefix_capable_crc32_device(payload, blocks, output_size, options);
    }

    record_gpu_decode_chunk(telemetry);

    if (decoded_crc_uses_contiguous_raw_payload(payload, blocks, output_size)) {
        std::byte* device_payload = nullptr;
        const auto payload_bytes = static_cast<std::size_t>(output_size);
        ensure_device_memory_budget(payload_bytes, "CRC raw payload");
        record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(payload_bytes));
        check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc CRC raw payload");
        try {
            check_hip(hipMemcpy(device_payload, payload.data(), payload_bytes, hipMemcpyHostToDevice),
                      "hipMemcpy CRC raw payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload_bytes));
            const auto crc =
                compute_crc32_device(device_payload, output_size, telemetry, "decoded raw CRC device memory");
            check_hip(hipFree(device_payload), "hipFree CRC raw payload");
            return crc;
        } catch (...) {
            (void)hipFree(device_payload);
            throw;
        }
    }

    auto host_blocks = build_decode_device_blocks(blocks);

    std::byte* device_payload = nullptr;
    DeviceBlock* device_blocks = nullptr;
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes =
        checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "CRC decode block table");
    const auto required_bytes = checked_add_bytes(payload_bytes, block_table_bytes, "CRC decode device memory");
    ensure_device_memory_budget(required_bytes, "CRC decode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc CRC payload");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * host_blocks.size()), "hipMalloc CRC decode blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice),
                      "hipMemcpy CRC payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
        }
        check_hip(hipMemcpy(device_blocks, host_blocks.data(), sizeof(DeviceBlock) * host_blocks.size(),
                            hipMemcpyHostToDevice),
                  "hipMemcpy CRC decode blocks");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));

        const auto crc =
            compute_decoded_crc32_device(device_payload, device_blocks, static_cast<std::uint32_t>(host_blocks.size()),
                                         output_size, telemetry, "decoded CRC device memory");
        check_hip(hipFree(device_payload), "hipFree CRC payload");
        check_hip(hipFree(device_blocks), "hipFree CRC decode blocks");
        return crc;
    } catch (...) {
        (void)hipFree(device_payload);
        (void)hipFree(device_blocks);
        throw;
    }
}

}  // namespace superzip
