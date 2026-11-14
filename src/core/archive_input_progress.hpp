#pragma once

#include "core/bounded_text_io.hpp"
#include "core/progress.hpp"

namespace flitzzip {

// Track physical encoded input, including metadata, line endings and discarded
// whitespace. Borrow one synchronous observer without copying its target. Input
// work is reported in bounded windows; finish() belongs after decoder validation
// and output close, before publication. This class owns no stream or output.
class ArchiveInputProgress {
  public:
    ArchiveInputProgress(ProgressState& progress, std::reference_wrapper<const ProgressCallback> callback,
                         std::uint64_t total, std::uint64_t initial = 0)
        : progress_(progress), callback_(callback.get()), total_(total), consumed_(initial) {
        if (initial > total) {
            throw ArchiveError("encoded input header exceeds its declared source length");
        }
    }
    ArchiveInputProgress(const ArchiveInputProgress&) = delete;
    ArchiveInputProgress& operator=(const ArchiveInputProgress&) = delete;

    void advance(std::uint64_t bytes) {
        if (bytes > total_ - consumed_) {
            throw ArchiveError("encoded input exceeds its declared source length");
        }
        consumed_ += bytes;
        if (consumed_ - reported_ >= kWindowBytes) {
            report();
        }
    }

    bool read_line(std::istream& input, std::string& line, std::size_t limit, std::string_view format) {
        std::size_t physical_bytes = 0;
        const bool present = read_bounded_text_line(input, line, limit, format, &physical_bytes);
        advance(physical_bytes);
        return present;
    }

    std::istream::int_type read_byte(std::istream& input) {
        const auto next = input.get();
        if (!std::char_traits<char>::eq_int_type(next, std::char_traits<char>::eof())) {
            advance(1U);
        }
        return next;
    }

    void finish() {
        if (consumed_ != total_) {
            throw ArchiveError("encoded input ended before its declared source length");
        }
        report();
    }

  private:
    void report() {
        progress_.add_bytes(consumed_ - reported_);
        reported_ = consumed_;
        publish_progress(progress_, callback_);
    }

    static constexpr std::uint64_t kWindowBytes = 65536U;
    ProgressState& progress_;
    const ProgressCallback& callback_;
    const std::uint64_t total_;
    std::uint64_t consumed_;
    std::uint64_t reported_ = 0;
};

}  // namespace flitzzip
