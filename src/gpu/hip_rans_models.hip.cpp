#include "gpu/hip_kernel_api.hpp"

namespace flitzzip::hip_detail {
namespace {

// Every caller admits the same one-dimensional 256-lane block before entry.
// All lanes initialize both arrays and participate in every barrier. Each sum
// is at most 256*u32_max, so the full-width mass never wraps.
__device__ void reduce_model_totals(std::uint64_t* mass, std::uint32_t* flags) {
    __syncthreads();
    for (unsigned int stride = 128U; stride != 0U; stride >>= 1U) {
        if (threadIdx.x < stride) {
            mass[threadIdx.x] += mass[threadIdx.x + stride];
            flags[threadIdx.x] += flags[threadIdx.x + stride];
        }
        __syncthreads();
    }
}

__device__ bool model_grid_is_valid() {
    return blockDim.x == kRansModelSymbols && blockDim.y == 1U && blockDim.z == 1U && gridDim.y == 1U &&
           gridDim.z == 1U;
}

__device__ bool model_rows_are_valid(std::uint32_t rows, std::uint32_t words) {
    return rows != 0U && rows <= 256U && (rows & (rows - 1U)) == 0U && words == rows * 256U;
}

// Rank positive symbols once per context, independent of precision. Equal raw
// counts use actual symbol order, making each output rank unique. Inactive
// sorted-list tails remain untouched and are never consumed.
__global__ void prepare_rans_models(const std::uint32_t* counts, std::uint32_t words, std::uint32_t bytes,
                                    std::uint8_t* order, std::uint32_t order_bytes, RansModelRow* rows,
                                    std::uint32_t row_count) {
    const auto row = blockIdx.x;
    const auto symbol = threadIdx.x;
    if (!model_grid_is_valid() || rows == nullptr || row >= row_count) {
        return;
    }
    if (symbol == 0U) {
        rows[row] = {0U, 0U, RansModelStatus::invalid_plan};
    }
    if (counts == nullptr || order == nullptr || !model_rows_are_valid(row_count, words) || order_bytes != words ||
        bytes == 0U || bytes > kRansCountMaxBytes) {
        return;
    }
    __shared__ std::uint32_t raw[256];
    __shared__ std::uint64_t mass[256];
    __shared__ std::uint32_t alphabet[256];
    const auto count = counts[row * 256U + symbol];
    raw[symbol] = count;
    mass[symbol] = count;
    alphabet[symbol] = count != 0U ? 1U : 0U;
    reduce_model_totals(mass, alphabet);
    if (mass[0] > bytes) {
        if (symbol == 0U) {
            rows[row].status = RansModelStatus::invalid_counts;
        }
        return;
    }
    if (count != 0U) {
        std::uint32_t rank = 0U;
        for (unsigned int other = 0U; other < 256U; ++other) {
            rank += raw[other] != 0U && (raw[other] < count || (raw[other] == count && other < symbol)) ? 1U : 0U;
        }
        order[row * 256U + rank] = static_cast<std::uint8_t>(symbol);
    }
    __syncthreads();
    if (symbol == 0U) {
        rows[row] = {static_cast<std::uint32_t>(mass[0]), alphabet[0], RansModelStatus::success};
    }
}

// Validate the complete positive permutation, not merely its first/last entry.
// Cardinality and mass are independently recounted by the calling block.
__device__ bool valid_model_order(const std::uint32_t* raw, const std::uint8_t* order, std::uint32_t alphabet,
                                  std::uint32_t lane) {
    if (lane >= alphabet) {
        return true;
    }
    const auto current = order[lane];
    if (raw[current] == 0U) {
        return false;
    }
    if (lane == 0U) {
        return true;
    }
    const auto previous = order[lane - 1U];
    return raw[previous] < raw[current] || (raw[previous] == raw[current] && previous < current);
}

// The sorted floor walk is scalar bounded control, shared by all lanes. Removing
// a sub-unit quota can only decrease the remaining scale, so the final threshold
// also identifies every previously fixed symbol without an inverse permutation.
__device__ void rans_floor_budget(const std::uint32_t* raw, const std::uint8_t* order, const RansModelRow& row,
                                  std::uint32_t precision, std::uint32_t& remaining_count,
                                  std::uint32_t& remaining_mass) {
    if (threadIdx.x == 0U) {
        remaining_count = row.source_mass;
        remaining_mass = 1U << precision;
        for (unsigned int i = 0U; i < row.alphabet_size; ++i) {
            const auto count = raw[order[i]];
            if (std::uint64_t{count} * remaining_mass >= remaining_count) {
                break;
            }
            remaining_count -= count;
            --remaining_mass;
        }
    }
    __syncthreads();
}

// Rank only the nonfixed quotas; ties use actual symbols, not sorted-count
// positions. No writes, barriers or dependence on another lane's progress.
__device__ std::uint32_t rans_remainder_rank(const std::uint32_t* eligible, const std::uint64_t* remainders,
                                             std::uint32_t symbol) {
    std::uint32_t rank = 0U;
    for (unsigned int other = 0U; other < 256U; ++other) {
        rank += eligible[other] != 0U && (remainders[other] > remainders[symbol] ||
                                          (remainders[other] == remainders[symbol] && other < symbol))
                    ? 1U
                    : 0U;
    }
    return rank;
}

// Exact integer water filling followed by largest remainders. The two ranks
// are independent: raw-count rank prepares the floor walk; remainder rank uses
// actual symbol order for ties. No floating-point quota or wave-size assumption.
__global__ void normalize_rans_models(const std::uint32_t* counts, std::uint32_t words, const std::uint8_t* order,
                                      std::uint32_t order_bytes, const RansModelRow* rows, std::uint32_t row_count,
                                      std::uint32_t bytes, std::uint32_t precision, std::uint32_t* frequencies,
                                      std::uint32_t frequency_words, RansModelStatus* statuses,
                                      std::uint32_t status_count) {
    const auto row = blockIdx.x;
    const auto symbol = threadIdx.x;
    if (!model_grid_is_valid() || statuses == nullptr || row >= status_count) {
        return;
    }
    if (symbol == 0U) {
        statuses[row] = RansModelStatus::invalid_plan;
    }
    if (counts == nullptr || order == nullptr || rows == nullptr || frequencies == nullptr ||
        !model_rows_are_valid(row_count, words) || order_bytes != words || frequency_words != words ||
        status_count != row_count || bytes == 0U || bytes > kRansCountMaxBytes || precision < 8U || precision > 16U) {
        return;
    }
    __shared__ std::uint32_t raw[256];
    __shared__ std::uint64_t sums[256];
    __shared__ std::uint32_t flags[256];
    __shared__ std::uint64_t remainders[256];
    __shared__ std::uint32_t eligible[256];
    __shared__ std::uint32_t remaining_count;
    __shared__ std::uint32_t remaining_mass;
    const auto count = counts[row * 256U + symbol];
    raw[symbol] = count;
    sums[symbol] = count;
    flags[symbol] = count != 0U ? 1U : 0U;
    reduce_model_totals(sums, flags);
    const auto metadata = rows[row];
    if (metadata.status != RansModelStatus::success || sums[0] > bytes || sums[0] != metadata.source_mass ||
        flags[0] != metadata.alphabet_size) {
        if (symbol == 0U) {
            statuses[row] = RansModelStatus::invalid_counts;
        }
        return;
    }
    // All lanes finish reading reduction results before reusing its scratch.
    __syncthreads();
    const auto* sorted = order + row * 256U;
    sums[symbol] = 0U;
    flags[symbol] = valid_model_order(raw, sorted, metadata.alphabet_size, symbol) ? 0U : 1U;
    reduce_model_totals(sums, flags);
    if (flags[0] != 0U) {
        if (symbol == 0U) {
            statuses[row] = RansModelStatus::invalid_model;
        }
        return;
    }
    rans_floor_budget(raw, sorted, metadata, precision, remaining_count, remaining_mass);
    const auto product = std::uint64_t{count} * remaining_mass;
    const bool apportioned = count != 0U && product >= remaining_count;
    const auto frequency = count == 0U ? 0U : apportioned ? static_cast<std::uint32_t>(product / remaining_count) : 1U;
    eligible[symbol] = apportioned ? 1U : 0U;
    remainders[symbol] = apportioned ? product % remaining_count : 0U;
    sums[symbol] = frequency;
    flags[symbol] = eligible[symbol];
    reduce_model_totals(sums, flags);
    const auto target = 1U << precision;
    // Empty contexts stay absent. Nonempty rows must have at least one
    // apportioned symbol; the sum deficit is strictly smaller than that set.
    if ((metadata.source_mass == 0U && sums[0] != 0U) ||
        (metadata.source_mass != 0U && (sums[0] > target || flags[0] == 0U || target - sums[0] >= flags[0]))) {
        if (symbol == 0U) {
            statuses[row] = RansModelStatus::invalid_model;
        }
        return;
    }
    const auto rank = apportioned ? rans_remainder_rank(eligible, remainders, symbol) : 0U;
    frequencies[row * 256U + symbol] = frequency + (apportioned && rank < target - sums[0] ? 1U : 0U);
    __syncthreads();
    if (symbol == 0U) {
        statuses[row] = RansModelStatus::success;
    }
}

// Admit frequency mass before publishing a CDF or reciprocal. The inclusive
// scan has uniform barriers and full-width arithmetic. Missing rows have zero
// offsets and deterministic zero storage; singleton mass is never narrowed.
__global__ void build_rans_models(const std::uint32_t* frequencies, std::uint32_t frequency_words,
                                  const RansModelRow* rows, std::uint32_t row_count, std::uint32_t precision,
                                  std::uint32_t* models, std::uint32_t model_words, std::uint32_t* reciprocals,
                                  std::uint32_t reciprocal_words, RansModelStatus* statuses,
                                  std::uint32_t status_count) {
    const auto row = blockIdx.x;
    const auto symbol = threadIdx.x;
    if (!model_grid_is_valid() || statuses == nullptr || row >= status_count) {
        return;
    }
    if (symbol == 0U) {
        statuses[row] = RansModelStatus::invalid_plan;
    }
    if (frequencies == nullptr || rows == nullptr || models == nullptr || reciprocals == nullptr ||
        !model_rows_are_valid(row_count, frequency_words) || reciprocal_words != frequency_words ||
        model_words != row_count * 258U || status_count != row_count || precision < 8U || precision > 16U) {
        return;
    }
    __shared__ std::uint64_t sums[256];
    __shared__ std::uint32_t alphabet[256];
    __shared__ std::uint32_t prefix[256];
    const auto frequency = frequencies[row * 256U + symbol];
    const auto metadata = rows[row];
    sums[symbol] = frequency;
    alphabet[symbol] = frequency != 0U ? 1U : 0U;
    prefix[symbol] = frequency;
    reduce_model_totals(sums, alphabet);
    const auto target = metadata.source_mass == 0U ? 0U : 1U << precision;
    if (metadata.status != RansModelStatus::success || metadata.source_mass > kRansCountMaxBytes || sums[0] != target ||
        alphabet[0] != metadata.alphabet_size) {
        if (symbol == 0U) {
            statuses[row] = RansModelStatus::invalid_model;
        }
        return;
    }
    for (unsigned int distance = 1U; distance < 256U; distance <<= 1U) {
        const auto add = symbol >= distance ? prefix[symbol - distance] : 0U;
        __syncthreads();
        prefix[symbol] += add;
        __syncthreads();
    }
    const auto offset = row_count + row * 257U;
    models[offset + symbol + 1U] = prefix[symbol];
    reciprocals[row * 256U + symbol] = frequency > 1U ? static_cast<std::uint32_t>((1ULL << 32U) / frequency) : 0U;
    if (symbol == 0U) {
        models[offset] = 0U;
    }
    __syncthreads();
    if (symbol == 0U) {
        models[row] = target != 0U ? offset : 0U;
        statuses[row] = RansModelStatus::success;
    }
}

}  // namespace

void bind_rans_model_kernels(HipKernelApi& api) noexcept {
    api.prepare_rans_models = prepare_rans_models;
    api.normalize_rans_models = normalize_rans_models;
    api.build_rans_models = build_rans_models;
}

}  // namespace flitzzip::hip_detail
