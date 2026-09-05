#include "zstd/zstd_runtime.hpp"

#include "core/result.hpp"

#include <sstream>
#include <string>

#if defined(_WIN32)
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
    compress_stream_ = load_required_symbol<CompressStreamFn>(module, "ZSTD_compressStream2");
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

ZstdCompressionContext* ZstdRuntime::create_compression_context() const {
    return create_compression_context_();
}

void ZstdRuntime::free_compression_context(ZstdCompressionContext* context) const noexcept {
    if (free_compression_context_ != nullptr) {
        (void)free_compression_context_(context);
    }
}

std::size_t ZstdRuntime::set_compression_parameter(ZstdCompressionContext* context, int parameter, int value) const {
    return set_compression_parameter_(context, parameter, value);
}

// Purpose: Advance one bounded Zstandard compression stream operation through the pinned runtime.
// Inputs: `context`, `output`, and `input` are caller-owned live buffers; `directive` selects continue or finalization.
// Outputs: Returns the Zstandard status code and updates the input/output positions through the runtime ABI.
std::size_t ZstdRuntime::compress_stream(ZstdCompressionContext* context, ZstdOutputBuffer* output,
                                         ZstdInputBuffer* input, ZstdEndDirective directive) const {
    return compress_stream_(context, output, input, directive);
}

ZstdDecompressionStream* ZstdRuntime::create_decompression_stream() const {
    return create_decompression_stream_();
}

void ZstdRuntime::free_decompression_stream(ZstdDecompressionStream* stream) const noexcept {
    if (free_decompression_stream_ != nullptr) {
        (void)free_decompression_stream_(stream);
    }
}

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

const ZstdRuntime& zstd_runtime() {
    static const ZstdRuntime runtime;
    return runtime;
}

}  // namespace superzip
