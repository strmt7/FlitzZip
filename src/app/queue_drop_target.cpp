#include "app/queue_drop_target.hpp"
#include "app/drop_payload.hpp"

#include <utility>

namespace flitzzip::app {

// Purpose: Bind the window and owned callbacks without calling external code.
// Inputs: Window and callbacks whose captured owners outlive registration.
// Outputs: Initializes a target with one owned COM reference.
QueueDropTarget::QueueDropTarget(HWND window, QueueDropCallbacks callbacks)
    : window_(window), callbacks_(std::move(callbacks)) {}

// Purpose: Expose supported COM interfaces with an owned reference.
// Inputs: Interface ID and writable output pointer. Outputs: COM status and interface pointer.
HRESULT STDMETHODCALLTYPE QueueDropTarget::QueryInterface(REFIID iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) {
        return E_NOINTERFACE;
    }
    *object = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
}

// Purpose: Retain a COM reference. Inputs: None. Outputs: New reference count.
ULONG STDMETHODCALLTYPE QueueDropTarget::AddRef() {
    return ++ref_count_;
}

// Purpose: Release a COM reference. Inputs: None. Outputs: Remaining count, deleting at zero.
ULONG STDMETHODCALLTYPE QueueDropTarget::Release() {
    const ULONG remaining = --ref_count_;
    if (remaining == 0U) {
        delete this;
    }
    return remaining;
}

// Purpose: Admit file drag tracking only when the source permits copying.
// Inputs: Borrowed data object, unused key state, screen point and in/out effect mask.
// Outputs: Permitted copy feedback or rejection; no exception crosses COM.
HRESULT STDMETHODCALLTYPE QueueDropTarget::DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) {
    if (effect == nullptr) {
        return reject(nullptr, "OLE drag enter requires an effect pointer", E_INVALIDARG);
    }
    const bool copy_allowed = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    try {
        data_object_has_files_ = copy_allowed && data_has_hdrop(data);
        return update_effect(point, effect, copy_allowed);
    } catch (...) {
        return reject(effect, "OLE drag enter failed");
    }
}

// Purpose: Recheck current source permission and geometry during drag tracking.
// Inputs: Unused key state, screen point and in/out effect mask.
// Outputs: Permitted copy feedback or rejection without payload access.
HRESULT STDMETHODCALLTYPE QueueDropTarget::DragOver(DWORD, POINTL point, DWORD* effect) {
    if (effect == nullptr) {
        return reject(nullptr, "OLE drag over requires an effect pointer", E_INVALIDARG);
    }
    const bool copy_allowed = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    try {
        return update_effect(point, effect, copy_allowed);
    } catch (...) {
        return reject(effect, "OLE drag over failed");
    }
}

// Purpose: End drag tracking even if a host callback fails.
// Inputs: None. Outputs: Clears cached availability and highlighting, returning S_OK.
HRESULT STDMETHODCALLTYPE QueueDropTarget::DragLeave() {
    data_object_has_files_ = false;
    try {
        callbacks_.highlight(false);
    } catch (...) {
        return reject(nullptr, "OLE drag leave failed");
    }
    return S_OK;
}

// Purpose: Queue a drop only after validating the source's permission and current target.
// Inputs: Borrowed data object, unused key state, screen point and in/out effect mask.
// Outputs: COPY only after successful host acceptance; failures leave NONE and no retained drag state.
HRESULT STDMETHODCALLTYPE QueueDropTarget::Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) {
    if (effect == nullptr) {
        return reject(nullptr, "OLE drop requires an effect pointer", E_INVALIDARG);
    }
    const bool copy_allowed = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    data_object_has_files_ = false;
    try {
        callbacks_.highlight(false);
        POINT client{};
        if (!copy_allowed || data == nullptr || !queue_point(point, client)) {
            return S_OK;
        }
        auto paths = paths_from_data_object(data);
        if (!paths.empty() && callbacks_.accept(std::move(paths), client)) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    } catch (...) {
        return reject(effect, "OLE drop failed");
    }
}

// Purpose: Isolate errors at the COM boundary without allowing cleanup failure to prevent logging.
// Inputs: Optional effect pointer, diagnostic text and return status.
// Outputs: Clears state, attempts each callback independently and returns the supplied status.
HRESULT QueueDropTarget::reject(DWORD* effect, const char* message, HRESULT status) noexcept {
    data_object_has_files_ = false;
    if (effect != nullptr) {
        *effect = DROPEFFECT_NONE;
    }
    try {
        callbacks_.highlight(false);
    } catch (...) {
    }
    try {
        callbacks_.report_failure(message);
    } catch (...) {
    }
    return status;
}

// Purpose: Query the supported shell format without fetching untrusted bytes.
// Inputs: Borrowed nullable data object. Outputs: True only for an exact successful format query.
bool QueueDropTarget::data_has_hdrop(IDataObject* data) {
    if (data == nullptr) {
        return false;
    }
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data->QueryGetData(&format) == S_OK;
}

// Purpose: Parse external file-drop storage once and release it on every successful acquisition path.
// Inputs: Borrowed nonnull data object. Outputs: A complete bounded path list or rejection.
std::vector<std::filesystem::path> QueueDropTarget::paths_from_data_object(IDataObject* data) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (data->GetData(&format, &medium) != S_OK) {
        return {};
    }
    struct MediumGuard {
        // Purpose: Own one acquired medium. Inputs: Borrowed medium reference. Outputs: Registers scope cleanup.
        explicit MediumGuard(STGMEDIUM& value) : medium(value) {}
        MediumGuard(const MediumGuard&) = delete;
        MediumGuard& operator=(const MediumGuard&) = delete;

        // Purpose: Release acquired OLE storage. Inputs: None. Outputs: Releases the medium exactly once.
        ~MediumGuard() {
            ReleaseStgMedium(&medium);
        }
        STGMEDIUM& medium;
    } guard{medium};
    if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) {
        return {};
    }
    return paths_from_dropfiles_global(medium.hGlobal);
}

// Purpose: Validate the native window before interpreting a screen point as queue coordinates.
// Inputs: Screen point and writable client point. Outputs: True only when conversion and hit testing succeed.
bool QueueDropTarget::queue_point(POINTL point, POINT& client) {
    client = {point.x, point.y};
    return window_ != nullptr && ScreenToClient(window_, &client) != FALSE && callbacks_.contains(client);
}

// Purpose: Publish feedback consistent with both source permission and queue acceptance.
// Inputs: Screen point, writable effect pointer and the source's copy permission.
// Outputs: Updates highlighting and writes only COPY or NONE, returning S_OK.
HRESULT QueueDropTarget::update_effect(POINTL point, DWORD* effect, bool copy_allowed) {
    POINT client{};
    const bool allowed = copy_allowed && data_object_has_files_ && queue_point(point, client);
    callbacks_.highlight(allowed);
    *effect = allowed ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

}  // namespace flitzzip::app
