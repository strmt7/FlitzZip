#include "gpu/hip_kernel_api.hpp"
#include "gpu/crc32_device.hpp"

namespace superzip {
namespace {

using namespace hip_detail;

// Purpose: Locate the decoded block containing one byte without scanning outside its table.
// Inputs: Validated decoded metadata, block count and output byte offset.
// Outputs: A bounded block index or block_count for inconsistent metadata.
__device__ std::uint32_t find_decoded_block(const DeviceBlock* blocks, std::uint32_t block_count,
                                            std::size_t output_offset);

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

}  // namespace

namespace hip_detail {
// Purpose: Apply one exact byte-plane permutation with one bounded lane per destination byte.
// Inputs: Distinct device spans of length bytes, an admitted width and forward/inverse direction.
// Outputs: Writes every destination once; incomplete final groups retain their original order.
__global__ void byte_plane_transform_kernel(const std::byte* source, std::byte* destination, std::uint32_t bytes,
                                            std::uint32_t width, bool inverse) {
    const auto index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= bytes) {
        return;
    }
    const auto rows = bytes / width;
    const auto full_bytes = rows * width;
    auto source_index = index;
    if (index < full_bytes) {
        // rows is positive here. Both mappings are bijections of [0, rows * width).
        source_index = inverse ? (index % width) * rows + index / width : (index % rows) * width + index / rows;
    }
    destination[index] = source[source_index];
}
}  // namespace hip_detail

// Purpose: Bind this translation unit's registered kernels to the private POD dispatch table.
// Inputs: Module-owned table under construction after DLL registration.
// Outputs: Writes only this component's typed entrypoints; allocates no storage.
void bind_codec_kernels(HipKernelApi& api) noexcept {
    api.materialize_prefix_segments_kernel = materialize_prefix_segments_kernel;
    api.verify_analysis_candidates_kernel = verify_analysis_candidates_kernel;
    api.materialize_blocks_kernel = materialize_blocks_kernel;
    api.materialize_segments_kernel = materialize_segments_kernel;
    api.apply_sparse_patches_kernel = apply_sparse_patches_kernel;
    api.crc32_segments_kernel = crc32_segments_kernel;
    api.crc32_independent_ranges_kernel = crc32_independent_ranges_kernel;
    api.decoded_crc32_segments_kernel = decoded_crc32_segments_kernel;
    api.diagnostic_compute_kernel = diagnostic_compute_kernel;
    api.diagnostic_checksum_kernel = diagnostic_checksum_kernel;
    api.byte_plane_transform = hip_detail::byte_plane_transform_kernel;
}

}  // namespace superzip
