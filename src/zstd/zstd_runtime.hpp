#pragma once

#include <cstddef>
#include <string>

namespace superzip {

struct ZstdCompressionContext;
struct ZstdDecompressionStream;

struct ZstdInputBuffer {
    const void* src;
    std::size_t size;
    std::size_t pos;
};

struct ZstdOutputBuffer {
    void* dst;
    std::size_t size;
    std::size_t pos;
};

enum class ZstdEndDirective : int {
    Continue = 0,
    Flush = 1,
    End = 2,
};

constexpr int kZstdCompressionLevelParameter = 100;
constexpr int kZstdContentChecksumParameter = 201;
constexpr int kZstdWindowLogMaxParameter = 100;

// Purpose: Own the bundled Zstandard DLL handle and expose the small stable C ABI SuperZip uses.
// Inputs: Constructed lazily from `zstd_runtime()` with no caller arguments.
// Outputs: Provides checked function dispatch to the app-local `libzstd.dll` runtime.
class ZstdRuntime final {
public:
    ZstdRuntime();
    ~ZstdRuntime();

    ZstdRuntime(const ZstdRuntime&) = delete;
    ZstdRuntime& operator=(const ZstdRuntime&) = delete;

    // Purpose: Create a compression context owned by the caller.
    // Inputs: None.
    // Outputs: Returns a Zstandard compression context pointer or null on runtime allocation failure.
    [[nodiscard]] ZstdCompressionContext* create_compression_context() const;

    // Purpose: Release a compression context obtained from `create_compression_context`.
    // Inputs: `context` is null or a runtime-owned compression context.
    // Outputs: Releases runtime memory; never throws.
    void free_compression_context(ZstdCompressionContext* context) const noexcept;

    // Purpose: Set an integer compression parameter on a live compression context.
    // Inputs: `context` is the target context, `parameter` is a Zstandard parameter id, `value` is the assigned value.
    // Outputs: Returns a Zstandard status code for caller validation.
    [[nodiscard]] std::size_t set_compression_parameter(ZstdCompressionContext* context, int parameter, int value) const;

    // Purpose: Compress one streaming chunk through the bundled runtime.
    // Inputs: `context`, `output`, `input`, and `directive` mirror the Zstandard stable C ABI.
    // Outputs: Returns remaining work or a Zstandard error code.
    [[nodiscard]] std::size_t compress_stream(
        ZstdCompressionContext* context,
        ZstdOutputBuffer* output,
        ZstdInputBuffer* input,
        ZstdEndDirective directive) const;

    // Purpose: Create a decompression stream owned by the caller.
    // Inputs: None.
    // Outputs: Returns a Zstandard decompression stream pointer or null on runtime allocation failure.
    [[nodiscard]] ZstdDecompressionStream* create_decompression_stream() const;

    // Purpose: Release a decompression stream obtained from `create_decompression_stream`.
    // Inputs: `stream` is null or a runtime-owned decompression stream.
    // Outputs: Releases runtime memory; never throws.
    void free_decompression_stream(ZstdDecompressionStream* stream) const noexcept;

    // Purpose: Set an integer decompression parameter on a live decompression stream.
    // Inputs: `stream` is the target stream, `parameter` is a Zstandard parameter id, `value` is the assigned value.
    // Outputs: Returns a Zstandard status code for caller validation.
    [[nodiscard]] std::size_t set_decompression_parameter(ZstdDecompressionStream* stream, int parameter, int value) const;

    // Purpose: Decompress one streaming chunk through the bundled runtime.
    // Inputs: `stream`, `output`, and `input` mirror the Zstandard stable C ABI.
    // Outputs: Returns remaining frame work or a Zstandard error code.
    [[nodiscard]] std::size_t decompress_stream(
        ZstdDecompressionStream* stream,
        ZstdOutputBuffer* output,
        ZstdInputBuffer* input) const;

    // Purpose: Test whether a runtime status code represents a Zstandard error.
    // Inputs: `code` is a value returned by a Zstandard runtime function.
    // Outputs: Returns true for error codes.
    [[nodiscard]] bool is_error(std::size_t code) const;

    // Purpose: Resolve a Zstandard error code into a stable runtime-owned diagnostic string.
    // Inputs: `code` is a value returned by a Zstandard runtime function.
    // Outputs: Returns the error name reported by the bundled runtime.
    [[nodiscard]] std::string error_name(std::size_t code) const;

private:
    using CreateCompressionContextFn = ZstdCompressionContext* (*)();
    using FreeCompressionContextFn = std::size_t (*)(ZstdCompressionContext*);
    using SetCompressionParameterFn = std::size_t (*)(ZstdCompressionContext*, int, int);
    using CompressStreamFn = std::size_t (*)(ZstdCompressionContext*, ZstdOutputBuffer*, ZstdInputBuffer*, ZstdEndDirective);
    using CreateDecompressionStreamFn = ZstdDecompressionStream* (*)();
    using FreeDecompressionStreamFn = std::size_t (*)(ZstdDecompressionStream*);
    using SetDecompressionParameterFn = std::size_t (*)(ZstdDecompressionStream*, int, int);
    using DecompressStreamFn = std::size_t (*)(ZstdDecompressionStream*, ZstdOutputBuffer*, ZstdInputBuffer*);
    using IsErrorFn = unsigned int (*)(std::size_t);
    using GetErrorNameFn = const char* (*)(std::size_t);
    using VersionNumberFn = unsigned int (*)();

    void* module_ = nullptr;
    CreateCompressionContextFn create_compression_context_ = nullptr;
    FreeCompressionContextFn free_compression_context_ = nullptr;
    SetCompressionParameterFn set_compression_parameter_ = nullptr;
    CompressStreamFn compress_stream_ = nullptr;
    CreateDecompressionStreamFn create_decompression_stream_ = nullptr;
    FreeDecompressionStreamFn free_decompression_stream_ = nullptr;
    SetDecompressionParameterFn set_decompression_parameter_ = nullptr;
    DecompressStreamFn decompress_stream_ = nullptr;
    IsErrorFn is_error_ = nullptr;
    GetErrorNameFn get_error_name_ = nullptr;
    VersionNumberFn version_number_ = nullptr;
};

// Purpose: Get the process-wide bundled Zstandard runtime.
// Inputs: None.
// Outputs: Returns a validated runtime object or throws if the app-local DLL is unavailable.
[[nodiscard]] const ZstdRuntime& zstd_runtime();

}  // namespace superzip
