#pragma once

#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <ios>
#include <limits>
#include <ostream>
#include <span>
#include <streambuf>

namespace flitzzip {

// Decoder-owned get areas remain valid until the next serialized source operation.
// Derived underflow implementations must publish such a window before returning
// a character. Standard reads and their existing putback area remain unchanged.
class DecodedInputBuffer : public std::streambuf {
  public:
    ~DecodedInputBuffer() override = default;
    DecodedInputBuffer(const DecodedInputBuffer&) = delete;
    DecodedInputBuffer& operator=(const DecodedInputBuffer&) = delete;

    // Visit one bounded owned window synchronously, without retaining the span or
    // reentering the source. Only a successful visitor consumes bytes. Source or
    // visitor exceptions propagate; zero maximum performs no source/visitor work.
    // A short positive result is a window boundary, not EOF. Ordinary stream
    // flags, gcount and the existing putback area are unchanged.
    template <class Consumer> [[nodiscard]] std::size_t consume(std::size_t maximum_bytes, Consumer&& consumer) {
        if (maximum_bytes == 0U) {
            return 0U;
        }
        if (traits_type::eq_int_type(sgetc(), traits_type::eof())) {
            return 0U;
        }
        if (gptr() == nullptr || egptr() == nullptr || gptr() >= egptr()) {
            throw ArchiveError("decoder did not publish a readable owned window");
        }
        const auto bytes = std::min({maximum_bytes, static_cast<std::size_t>(egptr() - gptr()),
                                     static_cast<std::size_t>(std::numeric_limits<int>::max())});
        std::invoke(consumer, std::span<const char>(gptr(), bytes));
        gbump(static_cast<int>(bytes));
        return bytes;
    }

    // Transfer one bounded window to a serialized synchronous sink without an
    // intermediate plaintext buffer. Zero result for a nonzero request means EOF.
    // Failed sinks throw ios_base::failure; source/sink exceptions propagate and
    // leave the current source window unconsumed. Callers own sink rollback.
    [[nodiscard]] std::size_t transfer_to(std::ostream& output, std::size_t maximum_bytes) {
        if (maximum_bytes == 0U) {
            return 0U;
        }
        if (!output) {
            throw std::ios_base::failure("decoded output sink is not writable");
        }
        return consume(maximum_bytes, [&](std::span<const char> bytes) {
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) {
                throw std::ios_base::failure("failed to write decoded output");
            }
        });
    }

  protected:
    DecodedInputBuffer() = default;
};

}  // namespace flitzzip
