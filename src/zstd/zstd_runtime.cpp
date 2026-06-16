#include "zstd/zstd_runtime.hpp"

#include "core/result.hpp"

#include <array>
#include <filesystem>
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
// Purpose: Format a Windows error code for diagnostics without exposing host-specific state.
// Inputs: `code` is a value returned by `GetLastError`.
// Outputs: Returns a compact ASCII diagnostic string.
std::string windows_error_text(DWORD code) {
    std::ostringstream out;
    out << "Windows error " << code;
    return out.str();
}

// Purpose: Resolve the directory that contains the running executable.
// Inputs: None.
// Outputs: Returns the executable directory or throws on Windows API failure.
std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
        throw ArchiveError("cannot locate SuperZip executable directory: " + windows_error_text(GetLastError()));
    }
    if (length >= buffer.size()) {
        throw ArchiveError("SuperZip executable path exceeds loader buffer");
    }
    return std::filesystem::path(buffer.data()).parent_path();
}

// Purpose: Load the bundled Zstandard runtime from the executable directory only.
// Inputs: None.
// Outputs: Returns a Windows module handle or throws with a concrete loader error.
HMODULE load_zstd_module() {
    const auto dll_path = executable_directory() / kZstdDllName;
    const auto handle = LoadLibraryExW(
        dll_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (handle == nullptr) {
        throw ArchiveError("cannot load bundled Zstandard runtime from " + dll_path.string() + ": " + windows_error_text(GetLastError()));
    }
    return handle;
}

// Purpose: Resolve one required Zstandard runtime export.
// Inputs: `module` is the loaded DLL handle and `name` is the exact C ABI symbol.
// Outputs: Returns the typed function pointer or throws when the runtime is incompatible.
template <typename Function>
Function load_required_symbol(HMODULE module, const char* name) {
    const auto proc = GetProcAddress(module, name);
    if (proc == nullptr) {
        throw ArchiveError(std::string("bundled Zstandard runtime is missing export: ") + name);
    }
    return reinterpret_cast<Function>(proc);
}
#endif

}  // namespace

ZstdRuntime::ZstdRuntime() {
#if defined(_WIN32)
    const auto module = load_zstd_module();
    try {
        module_ = module;
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
            out << "bundled Zstandard runtime version mismatch: expected " << kExpectedZstdVersionNumber << ", loaded " << version;
            throw ArchiveError(out.str());
        }
    } catch (...) {
        FreeLibrary(module);
        module_ = nullptr;
        throw;
    }
#else
    throw ArchiveError("bundled Zstandard runtime loading is available only in Windows x64 builds");
#endif
}

ZstdRuntime::~ZstdRuntime() {
#if defined(_WIN32)
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
#endif
}

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

std::size_t ZstdRuntime::compress_stream(
    ZstdCompressionContext* context,
    ZstdOutputBuffer* output,
    ZstdInputBuffer* input,
    ZstdEndDirective directive) const {
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

std::size_t ZstdRuntime::decompress_stream(
    ZstdDecompressionStream* stream,
    ZstdOutputBuffer* output,
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
