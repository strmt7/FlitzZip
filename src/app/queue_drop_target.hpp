#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>
#include <windows.h>
#include <oleidl.h>

namespace flitzzip::app {

// The window owns the target's registration and must revoke it before its callbacks expire.
// Accept must revalidate the queue state because an external data object can pump messages.
struct QueueDropCallbacks {
    std::function<bool(POINT)> contains;
    std::function<bool(std::vector<std::filesystem::path>, POINT)> accept;
    std::function<void(bool)> highlight;
    std::function<void(const char*)> report_failure;
};

class QueueDropTarget final : public IDropTarget {
  public:
    // Purpose: Bind the copy-only shell drop target to its window and queue callbacks.
    // Inputs: `window` remains valid during registration; all callbacks are required and owned by value.
    // Outputs: Creates one COM reference without registering the target or invoking callbacks.
    QueueDropTarget(HWND window, QueueDropCallbacks callbacks);

    QueueDropTarget(const QueueDropTarget&) = delete;
    QueueDropTarget& operator=(const QueueDropTarget&) = delete;

    // Purpose: Return a supported COM interface with one owned reference.
    // Inputs: `iid` selects an interface; `object` must be writable.
    // Outputs: Returns S_OK, E_POINTER or E_NOINTERFACE and clears unsupported output interfaces.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;

    // Purpose: Retain the target while COM uses it.
    // Inputs: None. Outputs: Returns the new reference count.
    ULONG STDMETHODCALLTYPE AddRef() override;

    // Purpose: Release a COM reference and destroy the target at zero.
    // Inputs: None. Outputs: Returns the remaining reference count.
    ULONG STDMETHODCALLTYPE Release() override;

    // Purpose: Check file availability and copy permission when a drag enters the queue.
    // Inputs: Borrowed external `data`, key state, screen `point`, and in/out allowed `effect` mask.
    // Outputs: Reports only permitted COPY or NONE; rejects null effects without querying external data.
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;

    // Purpose: Refresh queue feedback using the source's current operation mask.
    // Inputs: Key state, screen `point`, and in/out allowed `effect` mask.
    // Outputs: Reports only permitted COPY or NONE without fetching the payload.
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override;

    // Purpose: End drag tracking and remove highlighting.
    // Inputs: None. Outputs: Clears state without allowing exceptions to cross COM.
    HRESULT STDMETHODCALLTYPE DragLeave() override;

    // Purpose: Fetch and queue a bounded file list only for a permitted copy inside the queue.
    // Inputs: Borrowed external `data`, key state, screen `point`, and in/out allowed `effect` mask.
    // Outputs: Reports COPY only on acceptance; releases received storage on every path and rejects exceptions.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;

  private:
    // Purpose: Destroy a target only through its COM reference count.
    // Inputs: None. Outputs: Releases owned callbacks.
    ~QueueDropTarget() = default;

    // Purpose: Reject a callback failure while clearing drag state and recording diagnostics.
    // Inputs: Optional `effect`, diagnostic `message`, and HRESULT `status` to return.
    // Outputs: Reports NONE, attempts cleanup and logging independently, and never throws.
    HRESULT reject(DWORD* effect, const char* message, HRESULT status = S_OK) noexcept;

    // Purpose: Check the external object's advertised file format without reading its payload.
    // Inputs: Borrowed nullable `data`. Outputs: True only when CF_HDROP/HGLOBAL is available.
    static bool data_has_hdrop(IDataObject* data);

    // Purpose: Decode one externally owned CF_HDROP storage medium using the bounded parser.
    // Inputs: Borrowed nonnull `data`. Outputs: Owned paths or an empty list; storage is released exactly once.
    static std::vector<std::filesystem::path> paths_from_data_object(IDataObject* data);

    // Purpose: Resolve a valid queue client point before asking the host to accept input.
    // Inputs: Screen `point` and writable `client`. Outputs: True only for a live window and current queue hit.
    bool queue_point(POINTL point, POINT& client);

    // Purpose: Compute feedback from current source permission, data format and queue geometry.
    // Inputs: Screen `point`, writable `effect`, and `copy_allowed` from the input operation mask.
    // Outputs: Updates highlighting and reports COPY or NONE.
    HRESULT update_effect(POINTL point, DWORD* effect, bool copy_allowed);

    HWND window_;
    QueueDropCallbacks callbacks_;
    std::atomic_ulong ref_count_{1};
    bool data_object_has_files_ = false;
};

}  // namespace flitzzip::app
