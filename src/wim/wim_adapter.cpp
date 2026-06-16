#include "wim/wim_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <cwctype>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200)
#endif
#include "wimlib.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace superzip {
namespace {

constexpr std::size_t kWimCopyBufferBytes = 256U * 1024U;
constexpr std::uint64_t kMaxWimTotalFileBytes = kMaxPipelineMemoryBytes;

#if defined(_WIN32)
constexpr std::uint32_t kExpectedWimlibVersionNumber = 1062917U;
constexpr const wchar_t* kWimlibDllName = L"libwim-15.dll";

struct WimEntry {
    std::uint32_t image = 0;
    std::string source_path;
    std::string output_path;
    bool directory = false;
    std::uint64_t size = 0;
};

struct WimMetadata {
    std::vector<WimEntry> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint32_t image_count = 0;
};

// Purpose: Format a Windows error code for diagnostics without exposing unrelated host state.
// Inputs: `code` is a value returned by `GetLastError`.
// Outputs: Returns a compact ASCII diagnostic string.
std::string windows_error_text(DWORD code) {
    std::ostringstream out;
    out << "Windows error " << code;
    return out.str();
}

// Purpose: Resolve the directory containing the running SuperZip executable.
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

// Purpose: Convert a UTF-16 string returned by wimlib into UTF-8 for SuperZip archive path handling and diagnostics.
// Inputs: `value` is a null-terminated wimlib UTF-16 string.
// Outputs: Returns a UTF-8 string or throws if Windows reports invalid UTF-16 input.
std::string wide_to_utf8(const wchar_t* value) {
    if (value == nullptr) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw ArchiveError("wimlib returned invalid UTF-16 text: " + windows_error_text(GetLastError()));
    }
    std::string result(static_cast<std::size_t>(required - 1), '\0');
    if (!result.empty()) {
        const int written = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value,
            -1,
            result.data(),
            required,
            nullptr,
            nullptr);
        if (written != required) {
            throw ArchiveError("failed to convert wimlib UTF-16 text: " + windows_error_text(GetLastError()));
        }
    }
    return result;
}

class WimRuntime final {
public:
    using OpenWimFn = int (*)(const wimlib_tchar*, int, WIMStruct**);
    using GetWimInfoFn = int (*)(WIMStruct*, wimlib_wim_info*);
    using IterateDirTreeFn = int (*)(
        WIMStruct*,
        int,
        const wimlib_tchar*,
        int,
        wimlib_iterate_dir_tree_callback_t,
        void*);
    using ExtractImageFn = int (*)(WIMStruct*, int, const wimlib_tchar*, int);
    using FreeWimFn = void (*)(WIMStruct*);
    using ErrorStringFn = const wimlib_tchar* (*)(wimlib_error_code);
    using VersionNumberFn = std::uint32_t (*)();
    using GlobalInitFn = int (*)(int);
    using GlobalCleanupFn = void (*)();

    // Purpose: Load and validate the app-local wimlib runtime once for the process.
    // Inputs: None.
    // Outputs: Owns the checked DLL handle and required ABI symbols or throws on loader/version/init failure.
    WimRuntime() {
        const auto dll_path = executable_directory() / kWimlibDllName;
        const auto handle = LoadLibraryExW(
            dll_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (handle == nullptr) {
            throw ArchiveError("cannot load bundled wimlib runtime from " + dll_path.string() + ": " + windows_error_text(GetLastError()));
        }
        try {
            module_ = handle;
            open_wim_ = load_required_symbol<OpenWimFn>("wimlib_open_wim");
            get_wim_info_ = load_required_symbol<GetWimInfoFn>("wimlib_get_wim_info");
            iterate_dir_tree_ = load_required_symbol<IterateDirTreeFn>("wimlib_iterate_dir_tree");
            extract_image_ = load_required_symbol<ExtractImageFn>("wimlib_extract_image");
            free_wim_ = load_required_symbol<FreeWimFn>("wimlib_free");
            get_error_string_ = load_required_symbol<ErrorStringFn>("wimlib_get_error_string");
            version_number_ = load_required_symbol<VersionNumberFn>("wimlib_get_version");
            global_init_ = load_required_symbol<GlobalInitFn>("wimlib_global_init");
            global_cleanup_ = load_required_symbol<GlobalCleanupFn>("wimlib_global_cleanup");

            const auto version = version_number_();
            if (version != kExpectedWimlibVersionNumber) {
                std::ostringstream out;
                out << "bundled wimlib runtime version mismatch: expected "
                    << kExpectedWimlibVersionNumber << ", loaded " << version;
                throw ArchiveError(out.str());
            }
            const int init_status = global_init_(0);
            if (init_status != 0) {
                throw ArchiveError("wimlib initialization failed: " + error_message(init_status));
            }
        } catch (...) {
            FreeLibrary(handle);
            module_ = nullptr;
            throw;
        }
    }

    WimRuntime(const WimRuntime&) = delete;
    WimRuntime& operator=(const WimRuntime&) = delete;

    // Purpose: Release global wimlib resources and the runtime DLL at process shutdown.
    // Inputs: None.
    // Outputs: Frees resources owned by this runtime object; never throws.
    ~WimRuntime() {
        if (module_ != nullptr) {
            if (global_cleanup_ != nullptr) {
                global_cleanup_();
            }
            FreeLibrary(static_cast<HMODULE>(module_));
            module_ = nullptr;
        }
    }

    // Purpose: Open a WIM file with integrity checking and split-WIM rejection.
    // Inputs: `archive_path` is the archive path selected by the user.
    // Outputs: Returns a raw WIM handle owned by the caller or throws with a wimlib diagnostic.
    [[nodiscard]] WIMStruct* open_checked_wim(const std::filesystem::path& archive_path) const {
        WIMStruct* wim = nullptr;
        constexpr int kOpenFlags = WIMLIB_OPEN_FLAG_CHECK_INTEGRITY | WIMLIB_OPEN_FLAG_ERROR_IF_SPLIT;
        const int status = open_wim_(archive_path.wstring().c_str(), kOpenFlags, &wim);
        if (status != 0 || wim == nullptr) {
            throw ArchiveError("failed to open standalone WIM archive: " + error_message(status));
        }
        return wim;
    }

    // Purpose: Return general metadata for an open WIM handle.
    // Inputs: `wim` is a non-null handle returned by `open_checked_wim`.
    // Outputs: Returns wimlib's info structure or throws on API failure.
    [[nodiscard]] wimlib_wim_info info(WIMStruct* wim) const {
        wimlib_wim_info result{};
        const int status = get_wim_info_(wim, &result);
        if (status != 0) {
            throw ArchiveError("failed to read WIM metadata: " + error_message(status));
        }
        return result;
    }

    // Purpose: Iterate one image tree through wimlib.
    // Inputs: `wim`, `image`, `callback`, and `context` are passed directly to wimlib's recursive tree iterator.
    // Outputs: Returns wimlib status; callback-owned failures are carried in the callback context.
    [[nodiscard]] int iterate_image(
        WIMStruct* wim,
        std::uint32_t image,
        wimlib_iterate_dir_tree_callback_t callback,
        void* context) const {
        constexpr int kIterateFlags =
            WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE |
            WIMLIB_ITERATE_DIR_TREE_FLAG_CHILDREN |
            WIMLIB_ITERATE_DIR_TREE_FLAG_RESOURCES_NEEDED;
        return iterate_dir_tree_(wim, static_cast<int>(image), nullptr, kIterateFlags, callback, context);
    }

    // Purpose: Extract one validated WIM image into a private staging directory.
    // Inputs: `wim` is an open handle, `image` is one-based, and `target` is the staging root for that image.
    // Outputs: Writes staged files or throws with a wimlib diagnostic.
    void extract_image(WIMStruct* wim, std::uint32_t image, const std::filesystem::path& target) const {
        constexpr int kExtractFlags =
            WIMLIB_EXTRACT_FLAG_NO_ACLS |
            WIMLIB_EXTRACT_FLAG_NORPFIX |
            WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES;
        const int status = extract_image_(wim, static_cast<int>(image), target.wstring().c_str(), kExtractFlags);
        if (status != 0) {
            throw ArchiveError("failed to stage WIM image: " + error_message(status));
        }
    }

    // Purpose: Release an open WIM handle returned by this runtime.
    // Inputs: `wim` is null or a wimlib handle.
    // Outputs: Frees the handle; never throws.
    void free_wim(WIMStruct* wim) const noexcept {
        if (wim != nullptr && free_wim_ != nullptr) {
            free_wim_(wim);
        }
    }

    // Purpose: Resolve one wimlib status code into an ASCII diagnostic.
    // Inputs: `status` is a wimlib error code.
    // Outputs: Returns a stable error string suitable for exceptions.
    [[nodiscard]] std::string error_message(int status) const {
        const auto* message = get_error_string_(static_cast<wimlib_error_code>(status));
        auto text = wide_to_utf8(message);
        return text.empty() ? "unknown wimlib error" : text;
    }

private:
    // Purpose: Resolve one required wimlib export from the loaded runtime.
    // Inputs: `name` is the exact exported C ABI symbol.
    // Outputs: Returns the typed function pointer or throws when the runtime is incompatible.
    template <typename Function>
    [[nodiscard]] Function load_required_symbol(const char* name) const {
        const auto proc = GetProcAddress(static_cast<HMODULE>(module_), name);
        if (proc == nullptr) {
            throw ArchiveError(std::string("bundled wimlib runtime is missing export: ") + name);
        }
        return reinterpret_cast<Function>(proc);
    }

    void* module_ = nullptr;
    OpenWimFn open_wim_ = nullptr;
    GetWimInfoFn get_wim_info_ = nullptr;
    IterateDirTreeFn iterate_dir_tree_ = nullptr;
    ExtractImageFn extract_image_ = nullptr;
    FreeWimFn free_wim_ = nullptr;
    ErrorStringFn get_error_string_ = nullptr;
    VersionNumberFn version_number_ = nullptr;
    GlobalInitFn global_init_ = nullptr;
    GlobalCleanupFn global_cleanup_ = nullptr;
};

class WimHandle final {
public:
    // Purpose: Bind a runtime-owned WIM handle to RAII lifetime management.
    // Inputs: `runtime` must outlive this handle, and `handle` must be null or allocated by wimlib.
    // Outputs: Stores the handle for automatic release.
    WimHandle(const WimRuntime& runtime, WIMStruct* handle) : runtime_(runtime), handle_(handle) {}

    WimHandle(const WimHandle&) = delete;
    WimHandle& operator=(const WimHandle&) = delete;

    // Purpose: Release the WIM handle.
    // Inputs: None.
    // Outputs: Calls wimlib_free for the owned handle; never throws.
    ~WimHandle() {
        runtime_.free_wim(handle_);
    }

    // Purpose: Return the underlying WIM handle.
    // Inputs: None.
    // Outputs: Returns a non-null wimlib handle while this object is alive.
    [[nodiscard]] WIMStruct* get() const {
        return handle_;
    }

private:
    const WimRuntime& runtime_;
    WIMStruct* handle_ = nullptr;
};

// Purpose: Return the process-wide checked wimlib runtime.
// Inputs: None.
// Outputs: Returns a runtime object or throws if the app-local DLL cannot be loaded.
[[nodiscard]] const WimRuntime& wim_runtime() {
    static const WimRuntime runtime;
    return runtime;
}

// Purpose: Detect split-WIM path extensions that need multipart handling before extraction is safe.
// Inputs: `archive_path` is the user-selected archive path.
// Outputs: Returns true when the filename extension is `.swm` case-insensitively.
bool has_split_wim_extension(const std::filesystem::path& archive_path) {
    auto extension = archive_path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return extension == L".swm";
}

// Purpose: Add one file size to a bounded WIM extraction total.
// Inputs: `total` is the running total and `size` is the next uncompressed stream size.
// Outputs: Returns the updated total or throws before overflow/resource exhaustion.
std::uint64_t checked_add_wim_bytes(std::uint64_t total, std::uint64_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError("WIM uncompressed payload byte count overflows");
    }
    total += size;
    if (total > kMaxWimTotalFileBytes) {
        throw ArchiveError("WIM uncompressed payload exceeds SuperZip resource limit");
    }
    return total;
}

// Purpose: Normalize one wimlib image path into SuperZip's archive-path key form.
// Inputs: `entry` is untrusted metadata returned by wimlib.
// Outputs: Returns a slash-separated relative key or throws on malformed/unsafe path metadata.
std::string normalized_wim_source_path(const wimlib_dir_entry& entry) {
    if (entry.full_path == nullptr) {
        throw SecurityError("WIM entry is missing a full path");
    }
    auto raw = wide_to_utf8(entry.full_path);
    if (raw.size() >= 2U &&
        (raw[0] == '/' || raw[0] == '\\') &&
        (raw[1] == '/' || raw[1] == '\\')) {
        throw SecurityError("WIM entry uses a UNC-like path");
    }
    if (!raw.empty() && (raw.front() == '/' || raw.front() == '\\')) {
        raw.erase(raw.begin());
    }
    std::ranges::replace(raw, '\\', '/');
    return normalize_archive_path_key(raw);
}

// Purpose: Prefix a WIM member path when the archive contains multiple images.
// Inputs: `image` is one-based, `source_path` is already normalized, and `multi_image` indicates whether image folders are needed.
// Outputs: Returns the final archive path used below the destination.
std::string wim_output_path(std::uint32_t image, const std::string& source_path, bool multi_image) {
    if (!multi_image) {
        return source_path;
    }
    return "image-" + std::to_string(image) + "/" + source_path;
}

// Purpose: Reject WIM entry features that SuperZip does not publish safely.
// Inputs: `entry` is the untrusted wimlib directory entry being scanned.
// Outputs: Returns normally for directories and regular unnamed data streams; throws on links, reparse points, devices, named streams, or other unsupported attributes.
void validate_wim_entry_kind(const wimlib_dir_entry& entry) {
    constexpr std::uint32_t kUnsupportedAttributes =
        WIMLIB_FILE_ATTRIBUTE_DEVICE |
        WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT |
        WIMLIB_FILE_ATTRIBUTE_OFFLINE |
        WIMLIB_FILE_ATTRIBUTE_ENCRYPTED |
        WIMLIB_FILE_ATTRIBUTE_VIRTUAL;
    if ((entry.attributes & kUnsupportedAttributes) != 0U) {
        throw SecurityError("WIM entry uses unsupported Windows file attributes");
    }
    if (entry.num_links > 1U) {
        throw SecurityError("WIM hard links are not supported");
    }
    if (entry.num_named_streams != 0U) {
        throw SecurityError("WIM alternate data streams are not supported");
    }
    const bool directory = (entry.attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (!directory && entry.streams[0].stream_name != nullptr) {
        throw SecurityError("WIM regular file has an unexpected named default stream");
    }
    if (!directory && entry.streams[0].resource.is_missing) {
        throw ArchiveError("WIM file payload resource is missing");
    }
}

struct WimScanContext {
    WimMetadata* metadata = nullptr;
    std::vector<ArchivePathValidationEntry>* validation_entries = nullptr;
    std::uint32_t image = 0;
    bool multi_image = false;
    std::exception_ptr failure;
};

// Purpose: Record one validated WIM directory tree entry during wimlib iteration.
// Inputs: `context` receives metadata and `entry` is the untrusted wimlib entry.
// Outputs: Appends safe metadata or throws on resource-limit or security violations.
void record_wim_entry(WimScanContext& context, const wimlib_dir_entry& entry) {
    if (entry.depth == 0U) {
        return;
    }
    if (context.metadata == nullptr || context.validation_entries == nullptr) {
        throw ArchiveError("WIM scanner context is not initialized");
    }
    if (context.metadata->entries.size() >= kMaxArchiveEntries) {
        throw ArchiveError("WIM entry count exceeds SuperZip resource limit");
    }

    validate_wim_entry_kind(entry);
    const bool directory = (entry.attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) != 0U;
    const auto source_path = normalized_wim_source_path(entry);
    const auto output_path = wim_output_path(context.image, source_path, context.multi_image);
    const auto size = directory ? 0U : entry.streams[0].resource.uncompressed_size;
    if (!directory) {
        context.metadata->total_file_bytes = checked_add_wim_bytes(context.metadata->total_file_bytes, size);
    }
    context.metadata->entries.push_back(WimEntry{
        .image = context.image,
        .source_path = source_path,
        .output_path = output_path,
        .directory = directory,
        .size = size,
    });
    context.validation_entries->push_back(ArchivePathValidationEntry{
        .path = output_path,
        .directory = directory,
    });
}

// Purpose: Bridge wimlib's C callback into exception-safe C++ metadata collection.
// Inputs: `entry` is provided by wimlib for the active image and `user_context` points to `WimScanContext`.
// Outputs: Returns 0 on success or 1 after storing an exception for the caller to rethrow.
int wim_scan_callback(const wimlib_dir_entry* entry, void* user_context) noexcept {
    auto* context = static_cast<WimScanContext*>(user_context);
    try {
        if (entry == nullptr || context == nullptr) {
            throw ArchiveError("WIM scanner received an invalid callback");
        }
        record_wim_entry(*context, *entry);
        return 0;
    } catch (...) {
        if (context != nullptr) {
            context->failure = std::current_exception();
        }
        return 1;
    }
}

// Purpose: Scan all WIM images and validate archive-wide paths before extraction starts.
// Inputs: `runtime` dispatches wimlib calls and `archive_path` identifies the user-selected WIM archive.
// Outputs: Returns validated metadata or throws on malformed, unsupported, or unsafe archive contents.
WimMetadata scan_wim_metadata(const WimRuntime& runtime, const std::filesystem::path& archive_path) {
    WimHandle wim(runtime, runtime.open_checked_wim(archive_path));
    const auto info = runtime.info(wim.get());
    if (info.image_count == 0U) {
        throw ArchiveError("WIM archive contains no images");
    }
    if (info.image_count > 64U) {
        throw ArchiveError("WIM image count exceeds SuperZip resource limit");
    }

    WimMetadata metadata;
    metadata.image_count = info.image_count;
    std::vector<ArchivePathValidationEntry> validation_entries;
    const bool multi_image = info.image_count > 1U;
    for (std::uint32_t image = 1; image <= info.image_count; ++image) {
        WimScanContext context{
            .metadata = &metadata,
            .validation_entries = &validation_entries,
            .image = image,
            .multi_image = multi_image,
        };
        const int status = runtime.iterate_image(wim.get(), image, wim_scan_callback, &context);
        if (context.failure) {
            std::rethrow_exception(context.failure);
        }
        if (status != 0) {
            throw ArchiveError("failed to scan WIM image metadata: " + runtime.error_message(status));
        }
    }
    if (metadata.entries.empty()) {
        throw ArchiveError("WIM archive contains no extractable entries");
    }
    validate_archive_path_set(validation_entries);
    return metadata;
}

// Purpose: Join a normalized archive key under a trusted staging root.
// Inputs: `root` is a SuperZip-created staging directory and `normalized_path` has already passed `normalize_archive_path_key`.
// Outputs: Returns a filesystem path below `root`; this function does not validate untrusted input.
std::filesystem::path join_normalized_path(const std::filesystem::path& root, const std::string& normalized_path) {
    std::filesystem::path result = root;
    std::string component;
    for (const char ch : normalized_path) {
        if (ch == '/') {
            if (!component.empty()) {
                result /= component;
                component.clear();
            }
        } else {
            component.push_back(ch);
        }
    }
    if (!component.empty()) {
        result /= component;
    }
    return result;
}

// Purpose: Reserve a private staging directory for a wimlib image apply operation.
// Inputs: `destination` is the caller-selected extraction root.
// Outputs: Returns a unique staging directory below `destination` or throws on reservation failure.
ReservedFilePublishTarget reserve_wim_stage(const std::filesystem::path& destination) {
    return reserve_file_publish_target(destination / ".superzip-wim-stage");
}

// Purpose: Remove only a staging directory that follows SuperZip's WIM staging naming convention.
// Inputs: `stage` is the reserved staging directory returned by `reserve_wim_stage`.
// Outputs: Best-effort recursive cleanup of that private stage.
void cleanup_wim_stage(const ReservedFilePublishTarget& stage) {
    const auto name = stage.directory.filename().wstring();
    if (name.find(L".superzip-wim-stage.sztmp-") == std::wstring::npos) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(stage.directory, ignored);
}

// Purpose: Detect Windows reparse points in staged extraction output before publication.
// Inputs: `path` is a staged file path created by wimlib.
// Outputs: Returns true for reparse points or throws when attributes cannot be read.
bool is_reparse_point(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw ArchiveError("failed to inspect staged WIM file attributes: " + path.string());
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

// Purpose: Validate a staged file is an ordinary file with the expected size.
// Inputs: `source` is a staged path and `expected_size` is the prevalidated WIM stream size.
// Outputs: Returns normally for a safe staged file or throws on mismatch/special file.
void validate_staged_wim_file(const std::filesystem::path& source, std::uint64_t expected_size) {
    if (is_reparse_point(source)) {
        throw SecurityError("refusing to publish staged WIM reparse point: " + source.string());
    }
    std::error_code kind_error;
    if (!std::filesystem::is_regular_file(source, kind_error) || kind_error) {
        throw ArchiveError("staged WIM payload is not a regular file: " + source.string());
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(source, size_error);
    if (size_error || size != expected_size) {
        throw ArchiveError("staged WIM payload size changed after validation: " + source.string());
    }
}

// Purpose: Copy one staged WIM payload through the standard verified publication path.
// Inputs: `source` is the private staged file, `target` is the final safe output path, `expected_size` is the validated file size, and `overwrite` controls replacement.
// Outputs: Publishes the file atomically or throws after cleaning the temporary publication target.
void publish_staged_wim_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    std::uint64_t expected_size,
    bool overwrite) {
    validate_staged_wim_file(source, expected_size);
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    try {
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            throw ArchiveError("failed to open staged WIM payload: " + source.string());
        }
        std::ofstream output(temporary.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary WIM extraction target: " + target.string());
        }
        std::vector<char> buffer(kWimCopyBufferBytes);
        std::uint64_t copied = 0;
        while (copied < expected_size) {
            const auto remaining = expected_size - copied;
            const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(requested));
            const auto read = static_cast<std::size_t>(input.gcount());
            if (read == 0U) {
                throw ArchiveError("staged WIM payload ended early: " + source.string());
            }
            output.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!output) {
                throw ArchiveError("failed to write temporary WIM extraction target: " + target.string());
            }
            copied += read;
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary WIM extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }
}

// Purpose: Stage every WIM image through wimlib after metadata validation.
// Inputs: `runtime`, `archive_path`, and `metadata` identify the archive and image count; `stage` is the private root.
// Outputs: Writes staged files below image-specific stage directories or throws on wimlib failure.
void stage_wim_images(
    const WimRuntime& runtime,
    const std::filesystem::path& archive_path,
    const WimMetadata& metadata,
    const std::filesystem::path& stage) {
    WimHandle wim(runtime, runtime.open_checked_wim(archive_path));
    for (std::uint32_t image = 1; image <= metadata.image_count; ++image) {
        const auto image_stage = stage / ("image-" + std::to_string(image));
        std::filesystem::create_directories(image_stage);
        runtime.extract_image(wim.get(), image, image_stage);
    }
}

// Purpose: Publish validated staged WIM files into the requested destination.
// Inputs: `metadata` contains the prevalidated entries, `destination` is the extraction root, `stage` is the private staging root, and `overwrite`/`progress_callback` control publication.
// Outputs: Creates directories and verified files below `destination` or throws without intentionally deleting caller-owned output.
void publish_wim_entries(
    const WimMetadata& metadata,
    const std::filesystem::path& destination,
    const std::filesystem::path& stage,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    ProgressState progress;
    progress.start(OperationKind::Extract, metadata.total_file_bytes, metadata.entries.size());
    for (const auto& entry : metadata.entries) {
        progress.set_current(entry.output_path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.output_path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
        } else {
            const auto image_stage = stage / ("image-" + std::to_string(entry.image));
            const auto source = join_normalized_path(image_stage, entry.source_path);
            publish_staged_wim_file(source, target, entry.size, overwrite);
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
}
#endif

}  // namespace

OperationStats extract_wim(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
#if defined(_WIN32)
    const auto started = std::chrono::steady_clock::now();
    if (has_split_wim_extension(archive_path)) {
        throw ArchiveError("split WIM (.swm) extraction is not implemented yet; use a standalone .wim archive");
    }
    const auto& runtime = wim_runtime();
    const auto metadata = scan_wim_metadata(runtime, archive_path);
    std::filesystem::create_directories(destination);
    const auto stage = reserve_wim_stage(destination);
    bool stage_active = true;
    try {
        stage_wim_images(runtime, archive_path, metadata, stage.directory);
        publish_wim_entries(metadata, destination, stage.directory, overwrite, progress_callback);
        cleanup_wim_stage(stage);
        stage_active = false;
    } catch (...) {
        if (stage_active) {
            cleanup_wim_stage(stage);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
#else
    (void)archive_path;
    (void)destination;
    (void)overwrite;
    (void)progress_callback;
    throw ArchiveError("WIM extraction is available only in Windows x64 builds");
#endif
}

}  // namespace superzip
