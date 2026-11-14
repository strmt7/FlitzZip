#pragma once

#include "core/decoded_input_buffer.hpp"
#include "core/progress.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <istream>
#include <span>
#include <string>

namespace flitzzip {
namespace detail {

inline constexpr std::size_t kStreamConsumerWindowBytes = 64U * 1024U;

// Ordinary streams retain bounded read semantics. The temporary span is borrowed
// only during the consumer call; errors can follow already consumed windows.
template <class Consumer>
void consume_stream_bytes_buffered(std::istream& input, std::uint64_t remaining, Consumer& consumer, const char* label,
                                   const ProgressCheckpoint& checkpoint) {
    std::array<char, kStreamConsumerWindowBytes> buffer;
    while (remaining != 0U) {
        if (checkpoint) {
            checkpoint();
        }
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(bytes));
        if (input.gcount() != static_cast<std::streamsize>(bytes)) {
            throw ArchiveError(std::string(label) + " is truncated");
        }
        std::invoke(consumer, std::span<const char>(buffer.data(), bytes));
        remaining -= bytes;
    }
    if (checkpoint) {
        checkpoint();
    }
}

}  // namespace detail

// Consume exactly size bytes without retaining consumer spans. Zero size does no
// I/O; otherwise input must be readable. No consumer may reenter the source.
// Decoder windows are borrowed directly and not advanced if the consumer throws;
// ordinary streams read into bounded scratch storage before the consumer runs.
// Truncation, consumer and source errors propagate; earlier windows may be consumed.
// No whole-file allocation or hidden retry occurs. Output users own rollback and
// must qualify their sink batching rather than assuming a speedup from copy removal.
// Optional checkpoints run before each bounded window and after the final one.
// A checkpoint exception leaves the next unread window intact; earlier work remains.
template <class Consumer>
void consume_exact_stream_bytes(std::istream& input, std::uint64_t size, Consumer&& consumer, const char* label,
                                const ProgressCheckpoint& checkpoint = {}) {
    if (size == 0U) {
        return;
    }
    if (!input) {
        throw ArchiveError(std::string(label) + " stream is not readable");
    }
    if (auto* decoded = dynamic_cast<DecodedInputBuffer*>(input.rdbuf())) {
        auto remaining = size;
        while (remaining != 0U) {
            if (checkpoint) {
                checkpoint();
            }
            const auto limit =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, detail::kStreamConsumerWindowBytes));
            const auto bytes = decoded->consume(limit, consumer);
            if (bytes == 0U) {
                throw ArchiveError(std::string(label) + " is truncated");
            }
            remaining -= bytes;
        }
        if (checkpoint) {
            checkpoint();
        }
        return;
    }
    detail::consume_stream_bytes_buffered(input, size, consumer, label, checkpoint);
}

}  // namespace flitzzip
