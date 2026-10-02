#include "zstd/zstd_runtime.hpp"

#include "core/result.hpp"

#include <sstream>
#include <string>

#if defined(_WIN32)
#include "zstd_runtime_identity.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace superzip {
namespace {

constexpr unsigned int kExpectedZstdVersionNumber = 10507;
constexpr const wchar_t* kZstdDllName = L"libzstd.dll";

#if defined(_WIN32)
// Purpose: Resolve one required Zstandard runtime export.
// Inputs: `module` is the verified DLL handle and `name` is the exact C ABI symbol.
// Outputs: Returns the typed function pointer or throws when the runtime is incompatible.
template <typename Function> Function load_required_symbol(void* module, const char* name) {
    const auto proc = GetProcAddress(static_cast<HMODULE>(module), name);
    if (proc == nullptr) {
        throw ArchiveError(std::string("bundled Zstandard runtime is missing export: ") + name);
    }
    return reinterpret_cast<Function>(proc);
}
#endif

}  // namespace

ZstdRuntime::ZstdRuntime() : module_(load_trusted_app_local_runtime(kZstdDllName, SUPERZIP_ZSTD_RUNTIME_DLL_SHA256)) {
#if defined(_WIN32)
    const auto module = module_.native_handle();
    create_compression_context_ = load_required_symbol<CreateCompressionContextFn>(module, "ZSTD_createCCtx");
    free_compression_context_ = load_required_symbol<FreeCompressionContextFn>(module, "ZSTD_freeCCtx");
    set_compression_parameter_ = load_required_symbol<SetCompressionParameterFn>(module, "ZSTD_CCtx_setParameter");
    set_compression_source_size_ =
        load_required_symbol<SetCompressionSourceSizeFn>(module, "ZSTD_CCtx_setPledgedSrcSize");
    compression_workspace_bytes_ = load_required_symbol<CompressionWorkspaceBytesFn>(module, "ZSTD_sizeof_CCtx");
    estimate_block_workspace_bytes_ =
        load_required_symbol<EstimateBlockWorkspaceBytesFn>(module, "ZSTD_estimateCCtxSize");
    compress_stream_ = load_required_symbol<CompressStreamFn>(module, "ZSTD_compressStream2");
    compress_block_ = load_required_symbol<CompressBlockFn>(module, "ZSTD_compress");
    compress_block_with_context_ = load_required_symbol<CompressBlockWithContextFn>(module, "ZSTD_compressCCtx");
    block_compress_bound_ = load_required_symbol<CompressBoundFn>(module, "ZSTD_compressBound");
    decompress_block_ = load_required_symbol<DecompressBlockFn>(module, "ZSTD_decompress");
    first_frame_size_ = load_required_symbol<FirstFrameSizeFn>(module, "ZSTD_findFrameCompressedSize");
    frame_content_size_ = load_required_symbol<FrameContentSizeFn>(module, "ZSTD_getFrameContentSize");
    create_decompression_stream_ = load_required_symbol<CreateDecompressionStreamFn>(module, "ZSTD_createDStream");
    free_decompression_stream_ = load_required_symbol<FreeDecompressionStreamFn>(module, "ZSTD_freeDStream");
    set_decompression_parameter_ = load_required_symbol<SetDecompressionParameterFn>(module, "ZSTD_DCtx_setParameter");
    decompress_stream_ = load_required_symbol<DecompressStreamFn>(module, "ZSTD_decompressStream");
    is_error_ = load_required_symbol<IsErrorFn>(module, "ZSTD_isError");
    get_error_name_ = load_required_symbol<GetErrorNameFn>(module, "ZSTD_getErrorName");
    version_number_ = load_required_symbol<VersionNumberFn>(module, "ZSTD_versionNumber");
    const auto version = version_number_();
    if (version != kExpectedZstdVersionNumber) {
        std::ostringstream out;
        out << "bundled Zstandard runtime version mismatch: expected " << kExpectedZstdVersionNumber << ", loaded "
            << version;
        throw ArchiveError(out.str());
    }
#endif
}

ZstdRuntime::~ZstdRuntime() = default;

// Purpose: Allocate an encoder context through the pinned runtime ABI.
// Inputs: None; this runtime owns the loaded function table.
// Outputs: Returns a caller-owned context or null; callers must check and free it with this runtime.
ZstdCompressionContext* ZstdRuntime::create_compression_context() const {
    return create_compression_context_();
}

// Purpose: Release a caller-owned encoder context through its matching runtime.
// Inputs: `context` is null or was allocated by this runtime; ownership is surrendered.
// Outputs: Frees runtime resources when the release function exists; never throws.
void ZstdRuntime::free_compression_context(ZstdCompressionContext* context) const noexcept {
    if (free_compression_context_ != nullptr) {
        (void)free_compression_context_(context);
    }
}

// Purpose: Apply one stable compression parameter through the pinned runtime.
// Inputs: A live context, parameter identifier, and integer value.
// Outputs: Returns the runtime status for caller validation.
std::size_t ZstdRuntime::set_compression_parameter(ZstdCompressionContext* context, int parameter, int value) const {
    return set_compression_parameter_(context, parameter, value);
}

// Purpose: Declare exact frame input size through the pinned runtime's stable API.
// Inputs: A live context and exact unsigned byte count; zero declares an empty frame.
// Outputs: Returns a runtime status for caller validation.
std::size_t ZstdRuntime::set_compression_source_size(ZstdCompressionContext* context, unsigned long long bytes) const {
    return set_compression_source_size_(context, bytes);
}

// Purpose: Query actual codec-owned compression workspace without process-wide memory noise.
// Inputs: A live context with no concurrent operation.
// Outputs: Returns the runtime's context allocation size, excluding application-owned buffers.
std::size_t ZstdRuntime::compression_workspace_bytes(const ZstdCompressionContext* context) const {
    return compression_workspace_bytes_(context);
}

// Purpose: Obtain the version-pinned one-shot context bound without allocating a compression context.
// Inputs: The maximum native effort level, with no dictionaries or internal Zstandard worker threads.
// Outputs: Returns bytes covering levels up to that maximum, or a runtime error code.
std::size_t ZstdRuntime::estimate_block_workspace_bytes(int maximum_level) const {
    return estimate_block_workspace_bytes_(maximum_level);
}

// Purpose: Advance one bounded Zstandard compression stream operation through the pinned runtime.
// Inputs: `context`, `output`, and `input` are caller-owned live buffers; `directive` selects continue or finalization.
// Outputs: Returns the Zstandard status code and updates the input/output positions through the runtime ABI.
std::size_t ZstdRuntime::compress_stream(ZstdCompressionContext* context, ZstdOutputBuffer* output,
                                         ZstdInputBuffer* input, ZstdEndDirective directive) const {
    return compress_stream_(context, output, input, directive);
}

// Purpose: Encode one independent native block with the pinned app-local runtime.
// Inputs: Caller-owned input/output buffers, their byte lengths, and a bounded effort level.
// Outputs: Returns the frame size or a runtime error code.
std::size_t ZstdRuntime::compress_block(void* destination, std::size_t capacity, const void* source,
                                        std::size_t source_size, int level) const {
    return compress_block_(destination, capacity, source, source_size, level);
}

// Purpose: Encode one native frame through a reusable Zstandard context without retaining input state.
// Inputs: One exclusively owned context, bounded caller buffers, and the requested effort level.
// Outputs: Returns the frame size or a runtime error code.
std::size_t ZstdRuntime::compress_block_with_context(ZstdCompressionContext* context, void* destination,
                                                     std::size_t capacity, const void* source, std::size_t source_size,
                                                     int level) const {
    return compress_block_with_context_(context, destination, capacity, source, source_size, level);
}

// Purpose: Query the pinned runtime for a safe one-shot output capacity.
// Inputs: A validated block length.
// Outputs: Returns the maximum encoded byte count or a runtime error code.
std::size_t ZstdRuntime::block_compress_bound(std::size_t source_size) const {
    return block_compress_bound_(source_size);
}

// Purpose: Decode one complete native block frame with the pinned app-local runtime.
// Inputs: Caller-owned input/output buffers and their exact byte lengths.
// Outputs: Returns decoded bytes or a runtime error code.
std::size_t ZstdRuntime::decompress_block(void* destination, std::size_t capacity, const void* source,
                                          std::size_t source_size) const {
    return decompress_block_(destination, capacity, source, source_size);
}

// Purpose: Find a frame boundary before accepting any untrusted trailing payload bytes.
// Inputs: A bounded encoded source and its byte length.
// Outputs: Returns first-frame bytes or a runtime error code.
std::size_t ZstdRuntime::first_frame_size(const void* source, std::size_t source_size) const {
    return first_frame_size_(source, source_size);
}

// Purpose: Read an untrusted frame's declared output size for exact-size admission.
// Inputs: A bounded encoded source and its byte length.
// Outputs: Returns declared bytes or the runtime's unknown/error sentinel.
unsigned long long ZstdRuntime::frame_content_size(const void* source, std::size_t source_size) const {
    return frame_content_size_(source, source_size);
}

ZstdDecompressionStream* ZstdRuntime::create_decompression_stream() const {
    return create_decompression_stream_();
}

void ZstdRuntime::free_decompression_stream(ZstdDecompressionStream* stream) const noexcept {
    if (free_decompression_stream_ != nullptr) {
        (void)free_decompression_stream_(stream);
    }
}

// Purpose: Set an explicit decoder parameter through the pinned runtime ABI.
// Inputs: A live caller-owned `stream`, parameter identifier, and integer `value`.
// Outputs: Returns runtime status; callers must check it before decoding untrusted data.
std::size_t ZstdRuntime::set_decompression_parameter(ZstdDecompressionStream* stream, int parameter, int value) const {
    return set_decompression_parameter_(stream, parameter, value);
}

// Purpose: Advance one bounded Zstandard decompression operation through the pinned runtime.
// Inputs: `stream`, `output`, and `input` are caller-owned live buffers whose positions may be advanced.
// Outputs: Returns the Zstandard status code and updates the input/output positions through the runtime ABI.
std::size_t ZstdRuntime::decompress_stream(ZstdDecompressionStream* stream, ZstdOutputBuffer* output,
                                           ZstdInputBuffer* input) const {
    return decompress_stream_(stream, output, input);
}

bool ZstdRuntime::is_error(std::size_t code) const {
    return is_error_(code) != 0U;
}

std::string ZstdRuntime::error_name(std::size_t code) const {
    const auto* name = get_error_name_(code);
    return name == nullptr ? "unknown Zstandard error" : name;
}

// Purpose: Lazily initialize the process-wide verified Zstandard runtime.
// Inputs: None; construction enforces pinned runtime identity and ABI requirements.
// Outputs: Returns a process-lifetime shared reference, or propagates initialization failure.
const ZstdRuntime& zstd_runtime() {
    static const ZstdRuntime runtime;
    return runtime;
}

}  // namespace superzip
