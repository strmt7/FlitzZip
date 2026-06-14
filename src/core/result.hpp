#pragma once

#include <stdexcept>
#include <string>

namespace superzip {

class Error : public std::runtime_error {
public:
    // Purpose: Construct a base SuperZip exception with a stable diagnostic message.
    // Inputs: `message` is safe-to-display diagnostic text.
    // Outputs: Initializes `std::runtime_error` state.
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

class SecurityError : public Error {
public:
    // Purpose: Construct an exception for rejected unsafe paths or trust-boundary violations.
    // Inputs: `message` describes the rejected security condition.
    // Outputs: Initializes `Error` state.
    explicit SecurityError(const std::string& message) : Error(message) {}
};

class ArchiveError : public Error {
public:
    // Purpose: Construct an exception for archive format, filesystem, or validation failures.
    // Inputs: `message` describes the failing archive operation.
    // Outputs: Initializes `Error` state.
    explicit ArchiveError(const std::string& message) : Error(message) {}
};

class GpuError : public Error {
public:
    // Purpose: Construct an exception for required GPU availability or execution failures.
    // Inputs: `message` describes the failing AMD HIP operation.
    // Outputs: Initializes `Error` state.
    explicit GpuError(const std::string& message) : Error(message) {}
};

}  // namespace superzip
