#include "gpu/gpu_codec.hpp"

#include "core/checksum.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <string>
#include <vector>
#include <hip/hip_runtime.h>

namespace superzip {
GpuInfo query_hip_gpu_info();

namespace {

constexpr std::uint32_t kPatternSampleBytes = 4096U;
constexpr std::uint32_t kCrcSegmentBytes = 64U * 1024U;

struct DeviceBlock {
    std::uint8_t kind;
    std::uint8_t fill_value;
    std::uint16_t reserved;
    std::uint32_t uncompressed_len;
    std::uint64_t encoded_offset;
    std::uint64_t output_offset;
    std::uint32_t encoded_len;
};

struct DeviceCrcSegment {
    std::uint32_t crc32;
    std::uint32_t length;
};

__device__ __constant__ std::uint32_t kDeviceCrc32Table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU, 0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U, 0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU, 0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU, 0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U, 0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U, 0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U, 0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U, 0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU, 0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U, 0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU, 0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU, 0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U, 0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U, 0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U, 0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U, 0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU, 0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U, 0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU, 0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU, 0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U, 0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U, 0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U, 0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U, 0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU, 0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U, 0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU, 0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU, 0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U, 0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U, 0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U, 0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U, 0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU,
};

struct HipEventPair {
    hipEvent_t start = nullptr;
    hipEvent_t stop = nullptr;

    HipEventPair() = default;
    HipEventPair(const HipEventPair&) = delete;
    HipEventPair& operator=(const HipEventPair&) = delete;

    HipEventPair(HipEventPair&& other) noexcept
        : start(other.start), stop(other.stop) {
        other.start = nullptr;
        other.stop = nullptr;
    }

    HipEventPair& operator=(HipEventPair&& other) noexcept {
        if (this != &other) {
            if (start != nullptr) {
                (void)hipEventDestroy(start);
            }
            if (stop != nullptr) {
                (void)hipEventDestroy(stop);
            }
            start = other.start;
            stop = other.stop;
            other.start = nullptr;
            other.stop = nullptr;
        }
        return *this;
    }

    ~HipEventPair() {
        if (start != nullptr) {
            (void)hipEventDestroy(start);
        }
        if (stop != nullptr) {
            (void)hipEventDestroy(stop);
        }
    }
};

// Purpose: Convert a HIP status into a SuperZip GPU exception.
// Inputs: `status` is a HIP API result and `action` labels the failing operation.
// Outputs: Returns on success; throws `GpuError` with the HIP error string on failure.
void check_hip(hipError_t status, const char* action) {
    if (status != hipSuccess) {
        throw GpuError(std::string(action) + ": " + hipGetErrorString(status));
    }
}

// Purpose: Create a start/stop HIP event pair for measuring device-side kernel time.
// Inputs: `action` labels diagnostics if event creation fails.
// Outputs: Returns owned HIP events that are destroyed automatically.
HipEventPair make_hip_event_pair(const char* action) {
    HipEventPair events;
    check_hip(hipEventCreate(&events.start), action);
    check_hip(hipEventCreate(&events.stop), action);
    return events;
}

// Purpose: Finish a measured HIP kernel interval and record telemetry.
// Inputs: `telemetry` is optional operation-owned counters, `events` contains recorded start/stop events, and `action` labels diagnostics.
// Outputs: Synchronizes the stop event, records one launch plus elapsed device milliseconds, and throws on HIP errors.
void finish_measured_kernel(GpuTelemetry* telemetry, const HipEventPair& events, const char* action) {
    check_hip(hipEventSynchronize(events.stop), action);
    float milliseconds = 0.0F;
    check_hip(hipEventElapsedTime(&milliseconds, events.start, events.stop), action);
    record_gpu_kernel_launch(telemetry, static_cast<double>(milliseconds));
}

// Purpose: Add two allocation byte counts with overflow detection.
// Inputs: `lhs` and `rhs` are host/device allocation sizes; `action` labels the failing GPU operation.
// Outputs: Returns the sum or throws `GpuError` before wraparound.
std::size_t checked_add_bytes(std::size_t lhs, std::size_t rhs, const char* action) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        throw GpuError(std::string(action) + ": allocation size overflow");
    }
    return lhs + rhs;
}

// Purpose: Multiply allocation element count by element size with overflow detection.
// Inputs: `count`, `bytes_per_item`, and `action` describe a device allocation.
// Outputs: Returns byte size or throws `GpuError` before wraparound.
std::size_t checked_multiply_bytes(std::size_t count, std::size_t bytes_per_item, const char* action) {
    if (bytes_per_item != 0 && count > std::numeric_limits<std::size_t>::max() / bytes_per_item) {
        throw GpuError(std::string(action) + ": allocation size overflow");
    }
    return count * bytes_per_item;
}

// Purpose: Ensure a HIP operation leaves reserved VRAM for the OS, display, and other processes.
// Inputs: `required_bytes` is the total planned device allocation and `action` labels the operation.
// Outputs: Returns normally when free VRAM can cover the allocation plus reserve; throws `GpuError` otherwise.
void ensure_device_memory_budget(std::size_t required_bytes, const char* action) {
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    check_hip(hipMemGetInfo(&free_bytes, &total_bytes), "hipMemGetInfo");
    const auto reserve_target = std::max<std::size_t>(
        static_cast<std::size_t>(kDeviceMemoryReserveFloorBytes),
        total_bytes / 20U);
    const auto reserve = std::min<std::size_t>(free_bytes / 4U, reserve_target);
    const auto usable_bytes = free_bytes - reserve;
    if (required_bytes > usable_bytes) {
        throw GpuError(std::string(action) + ": insufficient AMD GPU VRAM for bounded chunk");
    }
}

// Purpose: Validate block layout before launching the HIP decode kernel.
// Inputs: `payload`, `blocks`, `output`, and `block_size` are caller-provided decode spans.
// Outputs: Returns normally for a dense fill/raw layout; throws `ArchiveError` before any kernel can read out of bounds.
void validate_decode_layout(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::size_t output_len,
    std::uint32_t /*block_size*/) {
    if (blocks.empty() && output_len != 0) {
        throw ArchiveError("decode block table is empty");
    }
    std::size_t out_pos = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        const auto len = static_cast<std::size_t>(block.uncompressed_len);
        if (len == 0) {
            throw ArchiveError("decode block has zero length");
        }
        if (len > kMaxArchiveBlockBytes) {
            throw ArchiveError("decode block exceeds SuperZip block size limit");
        }
        if (out_pos > output_len || len > output_len - out_pos) {
            throw ArchiveError("decode block exceeds output buffer");
        }
        if (block.kind == BlockKind::Fill) {
            if (block.encoded_len != 0) {
                throw ArchiveError("fill block contains encoded payload bytes");
            }
        } else if (block.kind == BlockKind::Raw) {
            if (block.encoded_len != block.uncompressed_len || block.encoded_offset > payload.size()) {
                throw ArchiveError("raw decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("raw decode block exceeds payload buffer");
            }
        } else if (block.kind == BlockKind::Deflate) {
            if (block.encoded_len == 0 || block.encoded_offset > payload.size()) {
                throw ArchiveError("deflate decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("deflate decode block exceeds payload buffer");
            }
        } else if (block.kind == BlockKind::Pattern) {
            if (block.encoded_len < 2 || block.encoded_len > kMaxGpuPatternBytes ||
                block.encoded_len >= block.uncompressed_len || block.encoded_offset > payload.size()) {
                throw ArchiveError("GPU pattern decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("GPU pattern decode block exceeds payload buffer");
            }
        } else {
            throw ArchiveError("decode block has unknown encoding kind");
        }
        out_pos += len;
    }
    if (out_pos != output_len) {
        throw ArchiveError("decode block sizes do not match output buffer");
    }
}

// Purpose: Classify each input block as uniform fill, repeated pattern, or raw bytes on the AMD GPU.
// Inputs: `input`/`input_len` are device bytes, `block_size` is bytes per block, `blocks` is device output metadata, and `block_count` bounds the launch.
// Outputs: Writes one `DeviceBlock` per block into device memory.
__global__ void analyze_blocks_kernel(
    const std::byte* input,
    std::size_t input_len,
    std::uint32_t block_size,
    DeviceBlock* blocks,
    std::uint32_t block_count) {
    const auto block_index = static_cast<std::uint32_t>(blockIdx.x);
    if (block_index >= block_count) {
        return;
    }
    const std::size_t start = static_cast<std::size_t>(block_index) * block_size;
    const std::size_t end = min(start + block_size, input_len);
    __shared__ int mismatch;
    __shared__ unsigned int first_value;
    __shared__ unsigned int selected_period;
    if (threadIdx.x == 0) {
        mismatch = 0;
        selected_period = 0;
        first_value = start < end ? static_cast<unsigned int>(input[start]) & 0xFFU : 0U;
    }
    __syncthreads();
    for (std::size_t pos = start + threadIdx.x; pos < end; pos += blockDim.x) {
        if ((static_cast<unsigned int>(input[pos]) & 0xFFU) != first_value) {
            atomicExch(&mismatch, 1);
        }
    }
    __syncthreads();
    if (threadIdx.x == 0) {
        const auto len = static_cast<std::uint32_t>(end - start);
        blocks[block_index].kind = mismatch == 0 ? 1 : 0;
        blocks[block_index].fill_value = static_cast<std::uint8_t>(first_value);
        blocks[block_index].reserved = 0;
        blocks[block_index].uncompressed_len = len;
        blocks[block_index].encoded_offset = 0;
        blocks[block_index].encoded_len = mismatch == 0 ? 0 : len;
    }
    __syncthreads();

    const auto len = static_cast<std::uint32_t>(end - start);
    if (mismatch == 0 || len < 4U) {
        return;
    }

    const auto max_period = min(static_cast<std::uint32_t>(kMaxGpuPatternBytes), len / 2U);
    const auto sample_len = min(len, static_cast<std::uint32_t>(kPatternSampleBytes));
    for (std::uint32_t period = 2; period <= max_period; ++period) {
        if (selected_period != 0U) {
            break;
        }
        if ((static_cast<unsigned int>(input[start + period]) & 0xFFU) != first_value) {
            continue;
        }
        if (threadIdx.x == 0) {
            mismatch = 0;
        }
        __syncthreads();
        for (std::uint32_t pos = period + threadIdx.x; pos < sample_len; pos += blockDim.x) {
            const auto actual = static_cast<unsigned int>(input[start + pos]) & 0xFFU;
            const auto expected = static_cast<unsigned int>(input[start + (pos % period)]) & 0xFFU;
            if (actual != expected) {
                atomicExch(&mismatch, 1);
            }
        }
        __syncthreads();
        if (mismatch != 0) {
            continue;
        }
        if (threadIdx.x == 0) {
            mismatch = 0;
        }
        __syncthreads();
        for (std::uint32_t pos = sample_len + threadIdx.x; pos < len; pos += blockDim.x) {
            const auto actual = static_cast<unsigned int>(input[start + pos]) & 0xFFU;
            const auto expected = static_cast<unsigned int>(input[start + (pos % period)]) & 0xFFU;
            if (actual != expected) {
                atomicExch(&mismatch, 1);
            }
        }
        __syncthreads();
        if (mismatch == 0 && threadIdx.x == 0) {
            selected_period = period;
        }
        __syncthreads();
    }

    if (threadIdx.x == 0 && selected_period != 0U) {
        blocks[block_index].kind = 3;
        blocks[block_index].fill_value = 0;
        blocks[block_index].encoded_len = selected_period;
    }
}

// Purpose: Decode fill/raw block metadata into output bytes on the AMD GPU.
// Inputs: `payload`, `blocks`, `block_count`, `output`, and `output_len` are device pointers/counts validated by the host path.
// Outputs: Writes decoded bytes into `output`.
__global__ void materialize_blocks_kernel(
    const std::byte* payload,
    const DeviceBlock* blocks,
    std::uint32_t block_count,
    std::byte* output,
    std::size_t output_len) {
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

// Purpose: Compute one finalized CRC-32 value per fixed-size segment of a device buffer.
// Inputs: `input`/`input_len` describe device bytes, `segments` is a device output table, and `segment_count` bounds the launch.
// Outputs: Writes ordered segment CRCs and segment lengths for host-side GF(2) concatenation.
__global__ void crc32_segments_kernel(
    const std::byte* input,
    std::size_t input_len,
    DeviceCrcSegment* segments,
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

// Purpose: Run integer-heavy work over a device buffer for the standalone GPU diagnostic.
// Inputs: `data` is a device buffer of `words` 32-bit elements, `seed` changes each launch, and `rounds` controls arithmetic intensity.
// Outputs: Mutates every device word deterministically.
__global__ void diagnostic_compute_kernel(
    std::uint32_t* data,
    std::size_t words,
    std::uint32_t seed,
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
__global__ void diagnostic_checksum_kernel(
    const std::uint32_t* data,
    std::size_t words,
    unsigned long long* partials) {
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
// Inputs: `device_input` is an allocated device buffer, `input_len` is its byte length, and `telemetry` records HIP activity.
// Outputs: Returns the finalized CRC-32 while copying only compact segment metadata back to host memory.
std::uint32_t compute_crc32_device(
    const std::byte* device_input,
    std::uint64_t input_len,
    GpuTelemetry* telemetry,
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
        crc32_segments_kernel<<<grid, threads>>>(device_input, static_cast<std::size_t>(input_len), device_segments, segments);
        check_hip(hipGetLastError(), "launch crc32_segments_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record crc32_segments_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize crc32_segments_kernel");

        std::vector<DeviceCrcSegment> host_segments(segments);
        check_hip(hipMemcpy(host_segments.data(), device_segments, segment_bytes, hipMemcpyDeviceToHost), "hipMemcpy CRC segments");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
        check_hip(hipFree(device_segments), "hipFree CRC segments");
        return combine_crc_segments(host_segments);
    } catch (...) {
        (void)hipFree(device_segments);
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
    const auto required_bytes = checked_add_bytes(static_cast<std::size_t>(bytes), partial_bytes, "diagnostic device memory");
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
        check_hip(
            hipMemcpy(device_data, host.data(), static_cast<std::size_t>(bytes), hipMemcpyHostToDevice),
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
        diagnostic_checksum_kernel<<<blocks, threads, threads * sizeof(unsigned long long)>>>(
            device_data,
            words,
            device_partials);
        check_hip(hipGetLastError(), "launch diagnostic_checksum_kernel");
        check_hip(hipEventRecord(checksum_events.stop, nullptr), "record diagnostic_checksum_kernel stop");
        check_hip(hipEventSynchronize(checksum_events.stop), "synchronize diagnostic_checksum_kernel");
        float checksum_ms = 0.0F;
        check_hip(hipEventElapsedTime(&checksum_ms, checksum_events.start, checksum_events.stop), "time diagnostic_checksum_kernel");
        result.kernel_ms += static_cast<double>(checksum_ms);
        ++result.kernel_launches;

        std::vector<unsigned long long> partials(blocks);
        check_hip(
            hipMemcpy(partials.data(), device_partials, partial_bytes, hipMemcpyDeviceToHost),
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
// Inputs: `input` is a bounded host chunk and `options` supplies block size plus telemetry.
// Outputs: Returns fill/raw/pattern block descriptors, encoded payload bytes, and GPU source CRC metadata.
EncodedChunk encode_chunk_hip(std::span<const std::byte> input, const GpuCodecOptions& options) {
    if (input.empty()) {
        return {};
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
    const auto block_table_bytes = checked_multiply_bytes(block_count, sizeof(DeviceBlock), "encode block table");
    const auto required_bytes = checked_add_bytes(input.size(), block_table_bytes, "encode device memory");
    ensure_device_memory_budget(required_bytes, "encode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));

    std::byte* device_input = nullptr;
    DeviceBlock* device_blocks = nullptr;
    check_hip(hipMalloc(&device_input, input.size()), "hipMalloc input");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * block_count), "hipMalloc block table");
    try {
        check_hip(hipMemcpy(device_input, input.data(), input.size(), hipMemcpyHostToDevice), "hipMemcpy input");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(input.size()));
        const auto source_crc32 = compute_crc32_device(
            device_input,
            static_cast<std::uint64_t>(input.size()),
            telemetry,
            "encode CRC device memory");
        auto events = make_hip_event_pair("create analyze_blocks_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record analyze_blocks_kernel start");
        analyze_blocks_kernel<<<block_count, 256>>>(device_input, input.size(), block_size, device_blocks, block_count);
        check_hip(hipGetLastError(), "launch analyze_blocks_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record analyze_blocks_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize analyze_blocks_kernel");

        std::vector<DeviceBlock> device_results(block_count);
        check_hip(
            hipMemcpy(device_results.data(), device_blocks, sizeof(DeviceBlock) * block_count, hipMemcpyDeviceToHost),
            "hipMemcpy block table");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));

        EncodedChunk out;
        out.source_crc32 = source_crc32;
        out.source_crc32_available = true;
        out.blocks.reserve(block_count);
        out.payload.reserve(input.size());
        std::uint64_t encoded_offset = 0;
        bool all_raw = true;
        std::uint64_t pattern_blocks = 0;
        for (std::uint32_t i = 0; i < block_count; ++i) {
            const auto len = static_cast<std::size_t>(device_results[i].uncompressed_len);
            if (device_results[i].kind == 1) {
                all_raw = false;
                out.blocks.push_back(BlockDescriptor{
                    .kind = BlockKind::Fill,
                    .fill_value = device_results[i].fill_value,
                    .uncompressed_len = static_cast<std::uint32_t>(len),
                    .encoded_offset = encoded_offset,
                    .encoded_len = 0,
                });
            } else if (device_results[i].kind == 3) {
                all_raw = false;
                ++pattern_blocks;
                out.blocks.push_back(BlockDescriptor{
                    .kind = BlockKind::Pattern,
                    .fill_value = 0,
                    .uncompressed_len = static_cast<std::uint32_t>(len),
                    .encoded_offset = encoded_offset,
                    .encoded_len = device_results[i].encoded_len,
                });
                encoded_offset += device_results[i].encoded_len;
            } else {
                out.blocks.push_back(BlockDescriptor{
                    .kind = BlockKind::Raw,
                    .fill_value = 0,
                    .uncompressed_len = static_cast<std::uint32_t>(len),
                    .encoded_offset = encoded_offset,
                    .encoded_len = static_cast<std::uint32_t>(len),
                });
                encoded_offset += len;
            }
        }
        record_gpu_pattern_blocks(telemetry, pattern_blocks);
        if (all_raw) {
            out.payload.resize(input.size());
            std::copy(input.begin(), input.end(), out.payload.begin());
        } else {
            for (std::uint32_t i = 0; i < block_count; ++i) {
                if (device_results[i].kind == 1) {
                    continue;
                }
                const auto start = static_cast<std::size_t>(i) * block_size;
                const auto len = static_cast<std::size_t>(device_results[i].uncompressed_len);
                const auto encoded_len = device_results[i].kind == 3
                    ? static_cast<std::size_t>(device_results[i].encoded_len)
                    : len;
                out.payload.insert(out.payload.end(), input.begin() + start, input.begin() + start + encoded_len);
            }
        }
        check_hip(hipFree(device_input), "hipFree input");
        check_hip(hipFree(device_blocks), "hipFree block table");
        return out;
    } catch (...) {
        (void)hipFree(device_input);
        (void)hipFree(device_blocks);
        throw;
    }
}

// Purpose: Decode GPU-supported block kinds into a caller-provided host buffer through AMD HIP.
// Inputs: `payload` and `blocks` are validated archive metadata, `output` is exact decoded storage, and `options` supplies telemetry.
// Outputs: Writes decoded bytes into `output`; deflate ranges are intentionally left for the CPU miniz path.
void decode_chunk_hip(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const GpuCodecOptions& options) {
    if (output.empty()) {
        return;
    }
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, output.size(), block_size);
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

    std::byte* device_payload = nullptr;
    std::byte* device_output = nullptr;
    DeviceBlock* device_blocks = nullptr;
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes = checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "decode block table");
    auto required_bytes = checked_add_bytes(payload_bytes, output.size(), "decode device memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "decode device memory");
    ensure_device_memory_budget(required_bytes, "decode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc payload");
    check_hip(hipMalloc(&device_output, output.size()), "hipMalloc output");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * host_blocks.size()), "hipMalloc decode blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice), "hipMemcpy payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
        }
        check_hip(
            hipMemcpy(device_blocks, host_blocks.data(), sizeof(DeviceBlock) * host_blocks.size(), hipMemcpyHostToDevice),
            "hipMemcpy decode blocks");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));
        constexpr int threads = 256;
        const auto grid = static_cast<unsigned int>(host_blocks.size());
        auto events = make_hip_event_pair("create materialize_blocks_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record materialize_blocks_kernel start");
        materialize_blocks_kernel<<<grid, threads>>>(
            device_payload,
            device_blocks,
            static_cast<std::uint32_t>(host_blocks.size()),
            device_output,
            output.size());
        check_hip(hipGetLastError(), "launch materialize_blocks_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record materialize_blocks_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize materialize_blocks_kernel");
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

// Purpose: Verify a decoded chunk by materializing GPU-supported blocks in VRAM and checksumming there.
// Inputs: `payload`/`blocks` describe encoded bytes, `output_size` is decoded byte count, and `options` supplies telemetry.
// Outputs: Returns ZIP-compatible CRC-32 while copying back only compact CRC segment metadata.
std::uint32_t crc_decoded_chunk_hip(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::uint64_t output_size,
    const GpuCodecOptions& options) {
    if (output_size == 0) {
        return 0;
    }
    if (output_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("decode output exceeds SuperZip resource limit");
    }
    auto* telemetry = options.telemetry.get();
    record_gpu_decode_chunk(telemetry);
    const auto info = query_hip_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    validate_decode_layout(payload, blocks, static_cast<std::size_t>(output_size), block_size);

    std::vector<DeviceBlock> host_blocks;
    host_blocks.reserve(blocks.size());
    std::uint64_t output_offset = 0;
    for (const auto& block : blocks) {
        if (block.kind == BlockKind::Deflate) {
            throw GpuError("GPU CRC-only verification does not support deflate blocks");
        }
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

    std::byte* device_payload = nullptr;
    std::byte* device_output = nullptr;
    DeviceBlock* device_blocks = nullptr;
    const auto payload_bytes = std::max<std::size_t>(payload.size(), 1);
    const auto block_table_bytes = checked_multiply_bytes(host_blocks.size(), sizeof(DeviceBlock), "CRC decode block table");
    auto required_bytes = checked_add_bytes(payload_bytes, static_cast<std::size_t>(output_size), "CRC decode device memory");
    required_bytes = checked_add_bytes(required_bytes, block_table_bytes, "CRC decode device memory");
    ensure_device_memory_budget(required_bytes, "CRC decode");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(hipMalloc(&device_payload, payload_bytes), "hipMalloc CRC payload");
    check_hip(hipMalloc(&device_output, static_cast<std::size_t>(output_size)), "hipMalloc CRC output");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * host_blocks.size()), "hipMalloc CRC decode blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice), "hipMemcpy CRC payload");
            record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(payload.size()));
        }
        check_hip(
            hipMemcpy(device_blocks, host_blocks.data(), sizeof(DeviceBlock) * host_blocks.size(), hipMemcpyHostToDevice),
            "hipMemcpy CRC decode blocks");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(block_table_bytes));

        constexpr int threads = 256;
        const auto grid = static_cast<unsigned int>(host_blocks.size());
        auto events = make_hip_event_pair("create CRC materialize_blocks_kernel events");
        check_hip(hipEventRecord(events.start, nullptr), "record CRC materialize_blocks_kernel start");
        materialize_blocks_kernel<<<grid, threads>>>(
            device_payload,
            device_blocks,
            static_cast<std::uint32_t>(host_blocks.size()),
            device_output,
            static_cast<std::size_t>(output_size));
        check_hip(hipGetLastError(), "launch CRC materialize_blocks_kernel");
        check_hip(hipEventRecord(events.stop, nullptr), "record CRC materialize_blocks_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize CRC materialize_blocks_kernel");

        const auto crc = compute_crc32_device(
            device_output,
            output_size,
            telemetry,
            "decoded CRC device memory");
        check_hip(hipFree(device_payload), "hipFree CRC payload");
        check_hip(hipFree(device_output), "hipFree CRC output");
        check_hip(hipFree(device_blocks), "hipFree CRC decode blocks");
        return crc;
    } catch (...) {
        (void)hipFree(device_payload);
        (void)hipFree(device_output);
        (void)hipFree(device_blocks);
        throw;
    }
}

}  // namespace superzip
