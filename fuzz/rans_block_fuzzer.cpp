#include "core/rans_block.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

// The parser retains its full production size limits. Only this harness's
// materialization is capped; two guarded vectors own less than 2 MiB together.
void decode_guarded(const flitzzip::GpuRansBlockView& block) {
    constexpr std::size_t kMaxDecodedBytes = 1024U * 1024U;
    if (block.decoded_size() > kMaxDecodedBytes) {
        return;
    }
    constexpr auto byte_guard = std::byte{0xA5};
    constexpr std::uint32_t word_guard = 0xA5A55A5AU;
    std::vector<std::byte> output(block.decoded_size() + 2U, byte_guard);
    std::vector<std::uint32_t> models(block.model_workspace_words() + 2U, word_guard);
    try {
        block.decode(std::span(output).subspan(1U, block.decoded_size()),
                     std::span(models).subspan(1U, block.model_workspace_words()));
    } catch (const flitzzip::ArchiveError&) {
        // Corrupt streams may alter the logical output/scratch, never guards.
    }
    if (output.front() != byte_guard || output.back() != byte_guard || models.front() != word_guard ||
        models.back() != word_guard) {
        std::abort();
    }
}

}  // namespace

// Four little-endian bytes supply decoded length; the remainder is the exact
// private entropy payload. Exercise the canonical parser and portable decoder,
// not archive admission or GPU execution. Unexpected exceptions remain failures.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < sizeof(std::uint32_t) || size > 1024U * 1024U) {
        return 0;
    }
    std::uint32_t decoded_bytes = 0U;
    for (std::size_t i = 0U; i < sizeof(decoded_bytes); ++i) {
        decoded_bytes |= static_cast<std::uint32_t>(data[i]) << (8U * i);
    }
    const auto payload = std::as_bytes(std::span(data, size)).subspan(sizeof(decoded_bytes));
    try {
        decode_guarded(flitzzip::GpuRansBlockView(payload, decoded_bytes));
    } catch (const flitzzip::ArchiveError&) {
    }
    return 0;
}
