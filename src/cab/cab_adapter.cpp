#include "cab/cab_adapter.hpp"

#include "cab/cab_format.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/progress.hpp"
#include "core/result.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <fdi.h>
#include <io.h>
#include <sys/stat.h>
#endif

namespace superzip {
namespace {

#ifdef _WIN32

constexpr const char* kCabFdiVirtualCabinetName = "superzip-input.cab";

enum class CabFdiPass {
    Validate,
    Extract,
};

struct CabValidatedEntry {
    std::uint64_t size = 0;
};

struct CabOutputTarget {
    ReservedFilePublishTarget temporary;
    std::filesystem::path final_path;
    std::string archive_path;
    std::uint64_t size = 0;
};

struct CabFdiContext {
    std::filesystem::path archive_path;
    std::filesystem::path destination;
    CabFdiPass pass = CabFdiPass::Validate;
    bool overwrite = false;
    bool progress_enabled = false;
    ProgressState progress;
    ProgressCallback progress_callback;
    std::unordered_map<std::string, CabValidatedEntry> entries;
    std::unordered_map<INT_PTR, CabOutputTarget> outputs;
    std::string error_message;
    bool security_error = false;
};

thread_local std::shared_ptr<CabFdiContext> g_cab_fdi_context;

class ScopedCabFdiContextBinding {
  public:
    // Purpose: Bind one CAB FDI context to the current thread for callback path remapping.
    // Inputs: `context` is the heap-owned CAB pass context used by callbacks.
    // Outputs: Restores the previous thread-local context when destroyed.
    explicit ScopedCabFdiContextBinding(std::shared_ptr<CabFdiContext> context)
        : previous_(std::move(g_cab_fdi_context)) {
        g_cab_fdi_context = std::move(context);
    }

    ScopedCabFdiContextBinding(const ScopedCabFdiContextBinding&) = delete;
    ScopedCabFdiContextBinding& operator=(const ScopedCabFdiContextBinding&) = delete;

    // Purpose: Restore the thread-local FDI context after one `FDICopy` call.
    // Inputs: None.
    // Outputs: Leaves the thread in its prior callback-binding state.
    ~ScopedCabFdiContextBinding() {
        g_cab_fdi_context = std::move(previous_);
    }

  private:
    std::shared_ptr<CabFdiContext> previous_;
};

// Purpose: Set a callback error once without throwing through the C FDI ABI.
// Inputs: `context` owns callback state and `message` is safe diagnostic text.
// Outputs: Stores the first error message for the caller to throw after `FDICopy`.
void set_cab_callback_error(CabFdiContext& context, std::string message, bool security_error = false) {
    if (context.error_message.empty()) {
        context.error_message = std::move(message);
        context.security_error = security_error;
    }
}

// Purpose: Return a stable diagnostics string for an FDI failure.
// Inputs: `perf` contains FDI error state from `FDICreate`/`FDICopy`.
// Outputs: Returns a compact message with FDI operation/type/native error fields.
std::string cab_fdi_error_message(const ERF& perf) {
    return "CAB decompression failed (fdi_error=" + std::to_string(perf.erfOper) +
           ", fdi_type=" + std::to_string(perf.erfType) + ", native_error=" + std::to_string(perf.fError) + ")";
}

// Purpose: Publish progress from inside a CAB callback.
// Inputs: `context` owns optional progress state and callback.
// Outputs: Calls the user's progress callback when progress tracking is active.
void publish_cab_progress(CabFdiContext& context) {
    if (context.progress_enabled && context.progress_callback) {
        publish_progress(context.progress, context.progress_callback);
    }
}

// Purpose: Convert one FDI file name to a prevalidated SuperZip path key.
// Inputs: `name` is an FDI-provided CAB filename pointer.
// Outputs: Returns a normalized path or throws on unsafe metadata.
std::string normalize_fdi_cab_name(const char* name) {
    if (name == nullptr) {
        throw ArchiveError("CAB file callback did not provide a name");
    }
    return normalize_cab_entry_path(name);
}

// Purpose: Allocate memory for the Windows FDI engine.
// Inputs: `cb` is the requested byte count.
// Outputs: Returns a C-heap allocation or null.
FNALLOC(cab_fdi_alloc) {
    const auto bytes = static_cast<std::size_t>(cb == 0 ? 1 : cb);
    return std::calloc(1U, bytes);
}

// Purpose: Free memory allocated by the Windows FDI engine.
// Inputs: `pv` is a pointer previously returned by `cab_fdi_alloc`.
// Outputs: Releases the allocation.
FNFREE(cab_fdi_free) {
    std::free(pv);
}

// Purpose: Open the virtual CAB path requested by FDI.
// Inputs: `pszFile` is the FDI path token, `oflag`/`pmode` are CRT open flags.
// Outputs: Returns a CRT file descriptor as `INT_PTR`, or -1 without opening attacker-named paths.
FNOPEN(cab_fdi_open) {
    if (g_cab_fdi_context != nullptr && std::strcmp(pszFile, kCabFdiVirtualCabinetName) == 0) {
        return static_cast<INT_PTR>(
            _wopen(g_cab_fdi_context->archive_path.wstring().c_str(), oflag | _O_BINARY, pmode));
    }
    if (g_cab_fdi_context != nullptr) {
        set_cab_callback_error(*g_cab_fdi_context, "CAB decompressor requested an unexpected external cabinet path");
    }
    return -1;
}

// Purpose: Read bytes for the Windows FDI engine.
// Inputs: `hf` is a CRT file descriptor, `pv` receives bytes, and `cb` is the requested byte count.
// Outputs: Returns bytes read, or `UINT_MAX` on CRT read failure.
FNREAD(cab_fdi_read) {
    const auto read = _read(static_cast<int>(hf), pv, cb);
    return read < 0 ? static_cast<UINT>(-1) : static_cast<UINT>(read);
}

// Purpose: Write decompressed bytes for the Windows FDI engine.
// Inputs: `hf` is a CRT file descriptor, `pv` provides bytes, and `cb` is the byte count.
// Outputs: Returns bytes written, or `UINT_MAX` on CRT write failure.
FNWRITE(cab_fdi_write) {
    const auto written = _write(static_cast<int>(hf), pv, cb);
    return written < 0 ? static_cast<UINT>(-1) : static_cast<UINT>(written);
}

// Purpose: Close a CRT file descriptor opened for FDI.
// Inputs: `hf` is a CRT file descriptor.
// Outputs: Returns the CRT close result.
FNCLOSE(cab_fdi_close) {
    return _close(static_cast<int>(hf));
}

// Purpose: Seek within a CRT file descriptor for FDI.
// Inputs: `hf` is a CRT file descriptor, `dist` is the requested offset, and `seektype` is the CRT seek origin.
// Outputs: Returns the resulting offset or -1 on failure.
FNSEEK(cab_fdi_seek) {
    return _lseek(static_cast<int>(hf), dist, seektype);
}

// Purpose: Open a discard sink for the CAB validation pass.
// Inputs: None.
// Outputs: Returns a CRT descriptor for the Windows NUL device or -1 on failure.
INT_PTR open_cab_discard_sink() {
    return static_cast<INT_PTR>(_open("NUL", _O_WRONLY | _O_BINARY));
}

// Purpose: Start extracting one validated CAB file to a private temporary file.
// Inputs: `context` owns validation/extraction state, `path` is normalized, and `size` is the FDI-reported uncompressed
// size. Outputs: Returns a writable FDI file descriptor or -1 after recording callback error.
INT_PTR open_cab_extract_target(CabFdiContext& context, const std::string& path, std::uint64_t size) {
    try {
        if (context.progress_enabled) {
            context.progress.set_current(path);
            publish_cab_progress(context);
        }
        const auto target = safe_join_archive_path(context.destination, path);
        std::filesystem::create_directories(target.parent_path());
        auto temporary = reserve_file_publish_target(target);
        const auto fd =
            _wopen(temporary.file.wstring().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd < 0) {
            cleanup_file_publish_target(temporary);
            set_cab_callback_error(context, "failed to create temporary CAB extraction target: " + target.string());
            return -1;
        }
        context.outputs.emplace(static_cast<INT_PTR>(fd), CabOutputTarget{
                                                              .temporary = std::move(temporary),
                                                              .final_path = target,
                                                              .archive_path = path,
                                                              .size = size,
                                                          });
        return static_cast<INT_PTR>(fd);
    } catch (const SecurityError& error) {
        set_cab_callback_error(context, error.what(), true);
        return -1;
    } catch (const std::exception& error) {
        set_cab_callback_error(context, error.what());
        return -1;
    }
}

// Purpose: Finish one FDI-emitted CAB file and publish it atomically.
// Inputs: `context` owns extraction state and `hf` is the FDI file descriptor.
// Outputs: Returns true on successful close/publish; records callback error and returns false otherwise.
bool close_and_publish_cab_output(CabFdiContext& context, INT_PTR hf) {
    auto it = context.outputs.find(hf);
    if (it == context.outputs.end()) {
        if (_close(static_cast<int>(hf)) != 0) {
            set_cab_callback_error(context, "failed to close CAB validation output");
            return false;
        }
        return true;
    }

    auto output = std::move(it->second);
    context.outputs.erase(it);
    if (_close(static_cast<int>(hf)) != 0) {
        cleanup_file_publish_target(output.temporary);
        set_cab_callback_error(context, "failed to close CAB extraction target: " + output.final_path.string());
        return false;
    }

    try {
        commit_verified_file(output.temporary.file, output.final_path, context.overwrite);
        cleanup_file_publish_target(output.temporary);
        if (context.progress_enabled) {
            context.progress.add_bytes(output.size);
            context.progress.finish_entry();
            publish_cab_progress(context);
        }
        return true;
    } catch (const SecurityError& error) {
        cleanup_file_publish_target(output.temporary);
        set_cab_callback_error(context, error.what(), true);
        return false;
    } catch (const std::exception& error) {
        cleanup_file_publish_target(output.temporary);
        set_cab_callback_error(context, error.what());
        return false;
    }
}

// Purpose: Handle Windows FDI extraction notifications.
// Inputs: `fdint` identifies the notification and `pfdin` carries FDI fields.
// Outputs: Returns FDI-compatible status/handle values without throwing through the C ABI.
FNFDINOTIFY(cab_fdi_notify) {
    auto* context = static_cast<CabFdiContext*>(pfdin == nullptr ? nullptr : pfdin->pv);
    if (context == nullptr) {
        return -1;
    }
    try {
        switch (fdint) {
        case fdintCABINET_INFO:
        case fdintENUMERATE:
            return 0;
        case fdintPARTIAL_FILE:
        case fdintNEXT_CABINET:
            set_cab_callback_error(*context, "spanned CAB sets are not supported");
            return -1;
        case fdintCOPY_FILE: {
            if (pfdin->cb < 0) {
                set_cab_callback_error(*context, "CAB file size is negative");
                return -1;
            }
            const auto path = normalize_fdi_cab_name(pfdin->psz1);
            const auto size = static_cast<std::uint64_t>(pfdin->cb);
            const auto entry = context->entries.find(path);
            if (entry == context->entries.end()) {
                set_cab_callback_error(*context, "CAB decompressor exposed an unvalidated path: " + path);
                return -1;
            }
            if (entry->second.size != size) {
                set_cab_callback_error(*context, "CAB decompressor size does not match validated metadata: " + path);
                return -1;
            }
            if (context->pass == CabFdiPass::Validate) {
                const auto sink = open_cab_discard_sink();
                if (sink < 0) {
                    set_cab_callback_error(*context, "failed to open CAB validation sink");
                }
                return sink;
            }
            return open_cab_extract_target(*context, path, size);
        }
        case fdintCLOSE_FILE_INFO:
            return close_and_publish_cab_output(*context, pfdin->hf) ? TRUE : FALSE;
        }
    } catch (const std::exception& error) {
        set_cab_callback_error(*context, error.what());
        return -1;
    }
    return 0;
}

class ScopedFdiContext {
  public:
    // Purpose: Create a Windows FDI context with SuperZip callback functions.
    // Inputs: None.
    // Outputs: Owns an HFDI handle or throws if FDI initialization fails.
    ScopedFdiContext() {
        handle_ = FDICreate(cab_fdi_alloc, cab_fdi_free, cab_fdi_open, cab_fdi_read, cab_fdi_write, cab_fdi_close,
                            cab_fdi_seek, cpuUNKNOWN, &error_);
        if (handle_ == nullptr) {
            throw ArchiveError(cab_fdi_error_message(error_));
        }
    }

    ScopedFdiContext(const ScopedFdiContext&) = delete;
    ScopedFdiContext& operator=(const ScopedFdiContext&) = delete;

    // Purpose: Destroy the Windows FDI context.
    // Inputs: None.
    // Outputs: Releases FDI state.
    ~ScopedFdiContext() {
        if (handle_ != nullptr) {
            FDIDestroy(handle_);
        }
    }

    // Purpose: Run one FDI copy/decompression pass.
    // Inputs: `context` carries pass-specific state and receives callback errors.
    // Outputs: Returns normally on success or throws on callback/FDI failure.
    void copy(const std::shared_ptr<CabFdiContext>& context) {
        ScopedCabFdiContextBinding binding(context);
        char cabinet_name[] = "superzip-input.cab";
        char cabinet_path[] = "";
        const BOOL ok = FDICopy(handle_, cabinet_name, cabinet_path, 0, cab_fdi_notify, nullptr, context.get());
        cleanup_pending_outputs(*context);
        if (!context->error_message.empty()) {
            if (context->security_error) {
                throw SecurityError(context->error_message);
            }
            throw ArchiveError(context->error_message);
        }
        if (ok == FALSE) {
            throw ArchiveError(cab_fdi_error_message(error_));
        }
    }

  private:
    // Purpose: Remove any extraction temporary files left after an FDI failure.
    // Inputs: `context` owns the temporary output map.
    // Outputs: Best-effort cleanup and map reset.
    static void cleanup_pending_outputs(CabFdiContext& context) {
        for (const auto& [handle, output] : context.outputs) {
            (void)_close(static_cast<int>(handle));
            cleanup_file_publish_target(output.temporary);
        }
        context.outputs.clear();
    }

    HFDI handle_ = nullptr;
    ERF error_{};
};

// Purpose: Build a lookup map from scanner-validated CAB entries.
// Inputs: `metadata` contains prevalidated entries.
// Outputs: Returns a path-to-size map used by FDI callbacks.
std::unordered_map<std::string, CabValidatedEntry> build_cab_entry_map(const CabMetadata& metadata) {
    std::unordered_map<std::string, CabValidatedEntry> entries;
    entries.reserve(metadata.entries.size());
    for (const auto& entry : metadata.entries) {
        entries.emplace(entry.path, CabValidatedEntry{.size = entry.size});
    }
    return entries;
}

// Purpose: Run one CAB FDI pass with the provided scanner metadata.
// Inputs: `archive_path` is the CAB, `destination` is used by extraction, `metadata` is prevalidated, `pass` selects
// validation or extraction, and progress fields are optional. Outputs: Returns normally on success or throws on
// decompression, path, I/O, or publication failures.
void run_cab_fdi_pass(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                      const CabMetadata& metadata, CabFdiPass pass, bool overwrite,
                      ProgressCallback progress_callback) {
    auto context = std::make_shared<CabFdiContext>();
    context->archive_path = archive_path;
    context->destination = destination;
    context->pass = pass;
    context->overwrite = overwrite;
    context->entries = build_cab_entry_map(metadata);
    if (progress_callback) {
        context->progress_enabled = true;
        context->progress_callback = std::move(progress_callback);
        context->progress.start(OperationKind::Extract, metadata.total_file_bytes, metadata.entries.size());
    }

    ScopedFdiContext fdi;
    fdi.copy(context);
}

#endif

}  // namespace

// Purpose: Extract a validated CAB archive through the Windows FDI engine.
// Inputs: `archive_path` is the user-selected CAB, `destination` receives safe entries, `overwrite` controls existing
// output replacement, and `progress_callback` receives extraction progress.
// Outputs: Returns extraction statistics or throws on unsupported platform, malformed CAB data, unsafe paths, or I/O
// failures.
OperationStats extract_cab(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
#ifndef _WIN32
    (void)archive_path;
    (void)destination;
    (void)overwrite;
    (void)progress_callback;
    throw ArchiveError("CAB extraction requires the Windows Cabinet API");
#else
    const auto started = std::chrono::steady_clock::now();
    const auto metadata = scan_cab_metadata(archive_path);

    run_cab_fdi_pass(archive_path, destination, metadata, CabFdiPass::Validate, overwrite, {});
    std::filesystem::create_directories(destination);

    run_cab_fdi_pass(archive_path, destination, metadata, CabFdiPass::Extract, overwrite, progress_callback);

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
#endif
}

}  // namespace superzip
