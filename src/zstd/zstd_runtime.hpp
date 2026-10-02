#pragma once

#include "core/trusted_runtime.hpp"

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
constexpr int kZstdCompressionWindowLogParameter = 101;
constexpr int kZstdCompressionHashLogParameter = 102;
constexpr int kZstdCompressionChainLogParameter = 103;
constexpr int kZstdContentSizeParameter = 200;
constexpr int kZstdContentChecksumParameter = 201;
constexpr int kZstdCompressionWorkersParameter = 400;
constexpr int kZstdWindowLogMaxParameter = 100;

// Purpose: Own the identity-pinned Zstandard DLL and expose its compression and workspace APIs.
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
    [[nodiscard]] std::size_t set_compression_parameter(ZstdCompressionContext* context, int parameter,
                                                        int value) const;

    // Purpose: Declare the exact next-frame input size so libzstd can size its compression workspace.
    // Inputs: A live context and exact byte count; zero means empty, not unknown.
    // Outputs: Returns a checked-by-caller runtime status; libzstd validates the pledge when finishing.
    [[nodiscard]] std::size_t set_compression_source_size(ZstdCompressionContext* context,
                                                          unsigned long long bytes) const;

    // Purpose: Report memory owned by a live compression context using the stable runtime ABI.
    // Inputs: A live context; no concurrent compression may mutate it during this call.
    // Outputs: Returns context/workspace bytes, excluding wrapper and caller buffers.
    [[nodiscard]] std::size_t compression_workspace_bytes(const ZstdCompressionContext* context) const;

    // Purpose: Bound one-shot context storage for all effort levels through the requested maximum.
    // Inputs: A validated maximum level; applies only to dictionary-free, single-thread compressCCtx calls.
    // Outputs: Returns the pinned runtime's worst-input estimate or a Zstandard error code.
    // The upstream estimate API is experimental; DLL identity and version validation are mandatory.
    [[nodiscard]] std::size_t estimate_block_workspace_bytes(int maximum_level) const;

    // Purpose: Compress one streaming chunk through the bundled runtime.
    // Inputs: `context`, `output`, `input`, and `directive` mirror the Zstandard stable C ABI.
    // Outputs: Returns remaining work or a Zstandard error code.
    [[nodiscard]] std::size_t compress_stream(ZstdCompressionContext* context, ZstdOutputBuffer* output,
                                              ZstdInputBuffer* input, ZstdEndDirective directive) const;

    // Purpose: Encode one independent bounded native block using a stable Zstandard frame.
    // Inputs: Caller-owned `source` and `destination` spans and a backend effort level.
    // Outputs: Returns bytes written or a Zstandard error code; never owns caller memory.
    [[nodiscard]] std::size_t compress_block(void* destination, std::size_t capacity, const void* source,
                                             std::size_t source_size, int level) const;

    // Purpose: Compress one independent frame while reusing a caller-owned context.
    // Inputs: `context` is exclusively owned by the calling worker; buffers and level match `compress_block`.
    // Outputs: Returns frame bytes or a Zstandard error code with the same wire format as `compress_block`.
    [[nodiscard]] std::size_t compress_block_with_context(ZstdCompressionContext* context, void* destination,
                                                          std::size_t capacity, const void* source,
                                                          std::size_t source_size, int level) const;

    // Purpose: Bound one-shot frame output before allocating a caller-owned candidate buffer.
    // Inputs: Validated source byte length.
    // Outputs: Returns the maximum encoded byte count or a Zstandard error code.
    [[nodiscard]] std::size_t block_compress_bound(std::size_t source_size) const;

    // Purpose: Decode one complete Zstandard frame into an exactly sized native block.
    // Inputs: Caller-owned encoded source and bounded decoded destination.
    // Outputs: Returns bytes written or a Zstandard error code.
    [[nodiscard]] std::size_t decompress_block(void* destination, std::size_t capacity, const void* source,
                                               std::size_t source_size) const;

    // Purpose: Locate the end of the first untrusted Zstandard frame.
    // Inputs: A bounded encoded block span.
    // Outputs: Returns the first frame's byte length or an error code for invalid framing.
    [[nodiscard]] std::size_t first_frame_size(const void* source, std::size_t source_size) const;

    // Purpose: Read the frame-declared decoded size before allocating or decoding a native block.
    // Inputs: A bounded encoded block span.
    // Outputs: Returns a size or Zstandard's unknown/error sentinel, which callers must reject.
    [[nodiscard]] unsigned long long frame_content_size(const void* source, std::size_t source_size) const;

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
    [[nodiscard]] std::size_t set_decompression_parameter(ZstdDecompressionStream* stream, int parameter,
                                                          int value) const;

    // Purpose: Decompress one streaming chunk through the bundled runtime.
    // Inputs: `stream`, `output`, and `input` mirror the Zstandard stable C ABI.
    // Outputs: Returns remaining frame work or a Zstandard error code.
    [[nodiscard]] std::size_t decompress_stream(ZstdDecompressionStream* stream, ZstdOutputBuffer* output,
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
    using SetCompressionSourceSizeFn = std::size_t (*)(ZstdCompressionContext*, unsigned long long);
    using CompressionWorkspaceBytesFn = std::size_t (*)(const ZstdCompressionContext*);
    using EstimateBlockWorkspaceBytesFn = std::size_t (*)(int);
    using CompressStreamFn = std::size_t (*)(ZstdCompressionContext*, ZstdOutputBuffer*, ZstdInputBuffer*,
                                             ZstdEndDirective);
    using CompressBlockFn = std::size_t (*)(void*, std::size_t, const void*, std::size_t, int);
    using CompressBlockWithContextFn = std::size_t (*)(ZstdCompressionContext*, void*, std::size_t, const void*,
                                                       std::size_t, int);
    using CompressBoundFn = std::size_t (*)(std::size_t);
    using DecompressBlockFn = std::size_t (*)(void*, std::size_t, const void*, std::size_t);
    using FirstFrameSizeFn = std::size_t (*)(const void*, std::size_t);
    using FrameContentSizeFn = unsigned long long (*)(const void*, std::size_t);
    using CreateDecompressionStreamFn = ZstdDecompressionStream* (*)();
    using FreeDecompressionStreamFn = std::size_t (*)(ZstdDecompressionStream*);
    using SetDecompressionParameterFn = std::size_t (*)(ZstdDecompressionStream*, int, int);
    using DecompressStreamFn = std::size_t (*)(ZstdDecompressionStream*, ZstdOutputBuffer*, ZstdInputBuffer*);
    using IsErrorFn = unsigned int (*)(std::size_t);
    using GetErrorNameFn = const char* (*)(std::size_t);
    using VersionNumberFn = unsigned int (*)();

    TrustedRuntimeModule module_;
    CreateCompressionContextFn create_compression_context_ = nullptr;
    FreeCompressionContextFn free_compression_context_ = nullptr;
    SetCompressionParameterFn set_compression_parameter_ = nullptr;
    SetCompressionSourceSizeFn set_compression_source_size_ = nullptr;
    CompressionWorkspaceBytesFn compression_workspace_bytes_ = nullptr;
    EstimateBlockWorkspaceBytesFn estimate_block_workspace_bytes_ = nullptr;
    CompressStreamFn compress_stream_ = nullptr;
    CompressBlockFn compress_block_ = nullptr;
    CompressBlockWithContextFn compress_block_with_context_ = nullptr;
    CompressBoundFn block_compress_bound_ = nullptr;
    DecompressBlockFn decompress_block_ = nullptr;
    FirstFrameSizeFn first_frame_size_ = nullptr;
    FrameContentSizeFn frame_content_size_ = nullptr;
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
