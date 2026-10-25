#pragma once

#include <iostream>
#include <sstream>

namespace flitzzip_test {

// Purpose: Share exception-safe stdout capture across serial native CLI tests.
// Inputs: The process's live stdout buffer; no other writer may run during capture.
// Outputs: Owns captured bytes and restores the original buffer before releasing its storage.
class StdoutCapture final {
  public:
    // Purpose: Borrow and redirect stdout. Inputs: Serial caller. Outputs: Capture active until destruction.
    StdoutCapture() : original_(std::cout.rdbuf(output_.rdbuf())) {}
    StdoutCapture(const StdoutCapture&) = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    // Purpose: Restore the borrowed buffer on normal and exceptional exit. Inputs: Owned capture. Outputs: Detached.
    ~StdoutCapture() {
        std::cout.rdbuf(original_);
    }

    // Purpose: Inspect captured bytes. Inputs: Live capture. Outputs: Independent readable stream.
    std::istringstream input() const {
        return std::istringstream(output_.str());
    }

    // Purpose: Compare exact records. Inputs: Live capture. Outputs: Owned byte string, never a borrowed buffer.
    std::string text() const {
        return output_.str();
    }

  private:
    std::ostringstream output_;
    std::streambuf* original_;
};

}  // namespace flitzzip_test
