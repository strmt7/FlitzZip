#include "app/queue_drop_target.hpp"
#include "test_util.hpp"

#include <array>
#include <memory>
#include <shlobj.h>

namespace {

class TestWindow {
  public:
    // Purpose: Create a private invisible window for real screen-to-client conversion.
    // Inputs: None. Outputs: Owns a message-only HWND; throws if creation fails.
    TestWindow() {
        handle = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 400, 300, HWND_MESSAGE, nullptr, nullptr, nullptr);
        REQUIRE_TRUE(handle != nullptr);
    }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    // Purpose: Release the private window. Inputs: None. Outputs: Destroys only this fixture's HWND.
    ~TestWindow() {
        DestroyWindow(handle);
    }
    HWND handle = nullptr;
};

class FileData final : public IDataObject {
  public:
    // Purpose: Create a real wide shell file-list fixture with independent ownership accounting.
    // Inputs: None. Outputs: Owns a movable HGLOBAL containing two paths, with one caller reference.
    FileData() {
        constexpr wchar_t paths[] = L"C:\\sample\\alpha.txt\0C:\\sample\\beta.dat\0";
        handle_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + sizeof(paths));
        REQUIRE_TRUE(handle_ != nullptr);
        auto* header = static_cast<DROPFILES*>(GlobalLock(handle_));
        if (header == nullptr) {
            GlobalFree(handle_);
            handle_ = nullptr;
            throw std::runtime_error("Could not lock the shell fixture");
        }
        header->pFiles = sizeof(DROPFILES);
        header->fWide = TRUE;
        std::copy(std::begin(paths), std::end(paths), reinterpret_cast<wchar_t*>(header + 1));
        GlobalUnlock(handle_);
    }
    FileData(const FileData&) = delete;
    FileData& operator=(const FileData&) = delete;

    // Purpose: Release the fixture's storage. Inputs: None. Outputs: Frees its one owned global allocation.
    ~FileData() {
        GlobalFree(handle_);
    }

    // Purpose: Expose supported fixture COM interfaces. Inputs: ID and output. Outputs: Owned interface or error.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (value == nullptr) {
            return E_POINTER;
        }
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) {
            return E_NOINTERFACE;
        }
        *value = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }

    // Purpose: Track external ownership. Inputs: None. Outputs: Increments and returns the reference count.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }

    // Purpose: Observe medium release independently from path acceptance.
    // Inputs: None. Outputs: Decrements references and counts releases; the test owns the fixture lifetime.
    ULONG STDMETHODCALLTYPE Release() override {
        ++releases;
        return --references;
    }

    // Purpose: Render one controlled external medium and transfer an owner reference.
    // Inputs: Requested format and writable medium. Outputs: Configured medium, failure or injected exception.
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        ++reads;
        validate_format(format);
        if (throw_get) {
            throw std::runtime_error("Injected external GetData failure");
        }
        if (get_status != S_OK) {
            return get_status;
        }
        REQUIRE_TRUE(medium != nullptr);
        *medium = {};
        medium->tymed = medium_type;
        medium->hGlobal = null_handle ? nullptr : handle_;
        medium->pUnkForRelease = this;
        AddRef();
        if (on_read) {
            on_read();
        }
        return S_OK;
    }

    // Purpose: Refuse unsupported in-place rendering. Inputs: Unused. Outputs: E_NOTIMPL.
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return E_NOTIMPL;
    }

    // Purpose: Advertise a controlled file-format result without reading storage.
    // Inputs: Requested format. Outputs: Configured status or an injected external exception.
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        ++queries;
        validate_format(format);
        if (throw_query) {
            throw std::runtime_error("Injected external format-query failure");
        }
        return query_status;
    }

    // Purpose: Refuse unsupported canonicalization. Inputs: Unused. Outputs: E_NOTIMPL.
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override {
        return E_NOTIMPL;
    }

    // Purpose: Refuse writes to the fixture. Inputs: Unused. Outputs: E_NOTIMPL.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    // Purpose: Refuse unsupported enumeration. Inputs: Unused. Outputs: E_NOTIMPL.
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override {
        return E_NOTIMPL;
    }

    // Purpose: Refuse advisory connections. Inputs: Unused. Outputs: OLE_E_ADVISENOTSUPPORTED.
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    // Purpose: Refuse advisory disconnection. Inputs: Unused. Outputs: OLE_E_ADVISENOTSUPPORTED.
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    // Purpose: Refuse advisory enumeration. Inputs: Unused. Outputs: OLE_E_ADVISENOTSUPPORTED.
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    // Purpose: Replace the list offset with one beyond the allocation.
    // Inputs: None. Outputs: Creates a malformed external payload without changing ownership.
    void invalidate_payload() {
        auto* header = static_cast<DROPFILES*>(GlobalLock(handle_));
        REQUIRE_TRUE(header != nullptr);
        header->pFiles = static_cast<DWORD>(GlobalSize(handle_));
        GlobalUnlock(handle_);
    }

    ULONG references = 1;
    unsigned releases = 0;
    unsigned reads = 0;
    unsigned queries = 0;
    HRESULT query_status = S_OK;
    HRESULT get_status = S_OK;
    DWORD medium_type = TYMED_HGLOBAL;
    bool null_handle = false;
    bool throw_query = false;
    bool throw_get = false;
    std::function<void()> on_read;

  private:
    // Purpose: Verify the consumer requests the shell file-list contract.
    // Inputs: Borrowed format. Outputs: Throws on any unexpected format, medium or indexing request.
    static void validate_format(const FORMATETC* format) {
        REQUIRE_TRUE(format != nullptr);
        REQUIRE_EQ(format->cfFormat, CF_HDROP);
        REQUIRE_EQ(format->dwAspect, DVASPECT_CONTENT);
        REQUIRE_EQ(format->lindex, -1);
        REQUIRE_EQ(format->tymed, TYMED_HGLOBAL);
        REQUIRE_TRUE(format->ptd == nullptr);
    }
    HGLOBAL handle_ = nullptr;
};

struct ReleaseTarget {
    // Purpose: Release the fixture's target reference. Inputs: Owned target. Outputs: Releases once.
    void operator()(flitzzip::app::QueueDropTarget* target) const {
        target->Release();
    }
};

struct DropFixture {
    // Purpose: Bind observable queue callbacks to the actual production COM implementation.
    // Inputs: None. Outputs: Owns one invisible window and target without publishing any UI.
    DropFixture() : target(new flitzzip::app::QueueDropTarget(window.handle, callbacks())) {}

    // Purpose: Build host callbacks with independent acceptance, lifetime and failure observations.
    // Inputs: None. Outputs: Callbacks that capture this fixture, whose lifetime encloses the target.
    flitzzip::app::QueueDropCallbacks callbacks() {
        return {
            [this](POINT point) {
                ++hit_tests;
                if (throw_hit) {
                    throw std::runtime_error("Injected hit-test failure");
                }
                return point.x >= 10 && point.y >= 10 && point.x < 100 && point.y < 100;
            },
            [this](std::vector<std::filesystem::path> paths, POINT point) {
                ++accepts;
                accepted_point = point;
                if (throw_accept) {
                    throw std::runtime_error("Injected queue failure");
                }
                if (can_accept) {
                    received = std::move(paths);
                }
                return can_accept;
            },
            [this](bool active) {
                if (throw_highlight) {
                    throw std::runtime_error("Injected paint failure");
                }
                highlighted = active;
            },
            [this](const char* message) {
                ++warnings;
                REQUIRE_TRUE(message != nullptr && message[0] != '\0');
                if (throw_report) {
                    throw std::runtime_error("Injected logging failure");
                }
            },
        };
    }

    // Purpose: Obtain a native screen point from independent fixture client coordinates.
    // Inputs: Client point, defaulting inside the queue. Outputs: Exact Win32 screen coordinates.
    POINTL screen(POINT client = {30, 40}) const {
        REQUIRE_TRUE(ClientToScreen(window.handle, &client));
        return {client.x, client.y};
    }

    TestWindow window;
    std::unique_ptr<flitzzip::app::QueueDropTarget, ReleaseTarget> target;
    std::vector<std::filesystem::path> received;
    POINT accepted_point{};
    unsigned hit_tests = 0;
    unsigned accepts = 0;
    unsigned warnings = 0;
    bool highlighted = false;
    bool can_accept = true;
    bool throw_hit = false;
    bool throw_accept = false;
    bool throw_highlight = false;
    bool throw_report = false;
};

}  // namespace

// Purpose: Reject every source mask that does not authorize copying.
// Inputs: Real callbacks and move/link/scroll/unknown operation masks. Outputs: No payload reads or queue mutation.
TEST_CASE(queue_drop_target_honors_source_permissions) {
    constexpr std::array<DWORD, 6> masks{
        DROPEFFECT_NONE, DROPEFFECT_MOVE, DROPEFFECT_LINK, DROPEFFECT_MOVE | DROPEFFECT_LINK, DROPEFFECT_SCROLL, 0x10U};
    for (const DWORD mask : masks) {
        DropFixture fixture;
        FileData data;
        DWORD effect = mask;
        REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
        effect = mask;
        REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
        REQUIRE_EQ(data.queries, 0U);
        REQUIRE_EQ(data.reads, 0U);
        REQUIRE_EQ(fixture.accepts, 0U);
        REQUIRE_TRUE(!fixture.highlighted);
    }
}

// Purpose: Require valid in/out effect storage before any external data or queue access.
// Inputs: Null effect pointers on each callback. Outputs: E_INVALIDARG and cleared drag tracking.
TEST_CASE(queue_drop_target_rejects_null_effects) {
    DropFixture fixture;
    FileData data;
    DWORD effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_TRUE(fixture.highlighted);
    REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), nullptr), E_INVALIDARG);
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), nullptr), E_INVALIDARG);
    REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), nullptr), E_INVALIDARG);
    REQUIRE_EQ(data.queries, 1U);
    REQUIRE_EQ(data.reads, 0U);
    REQUIRE_EQ(fixture.accepts, 0U);
    REQUIRE_TRUE(!fixture.highlighted);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
}

// Purpose: Recheck effect permissions and target geometry throughout a drag without reading payloads.
// Inputs: Changing source masks and points inside/outside the queue. Outputs: Accurate feedback and no eager reads.
TEST_CASE(queue_drop_target_feedback_tracks_current_permissions_and_geometry) {
    DropFixture fixture;
    FileData data;
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    REQUIRE_EQ(fixture.target->DragEnter(&data, MK_SHIFT, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_COPY);
    REQUIRE_TRUE(fixture.highlighted);
    effect = DROPEFFECT_MOVE;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    REQUIRE_TRUE(!fixture.highlighted);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen({101, 40}), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    effect = DROPEFFECT_COPY | DROPEFFECT_LINK | DROPEFFECT_SCROLL;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_COPY);
    REQUIRE_EQ(fixture.target->DragLeave(), S_OK);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    REQUIRE_EQ(data.reads, 0U);
}

// Purpose: Reject inaccessible data and unavailable formats during drag entry.
// Inputs: Null data or failed/nonexact format-query status. Outputs: No accepted effect or payload access.
TEST_CASE(queue_drop_target_rejects_unavailable_file_formats) {
    DropFixture fixture;
    FileData data;
    for (const HRESULT status : {DV_E_FORMATETC, E_FAIL, S_FALSE}) {
        data.query_status = status;
        DWORD effect = DROPEFFECT_COPY;
        REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
    }
    DWORD effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragEnter(nullptr, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->Drop(nullptr, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    REQUIRE_EQ(data.reads, 0U);
}

// Purpose: Reject a final drop outside the queue even after valid entry.
// Inputs: A source that changes its operation mask or cursor location before Drop.
// Outputs: Neither forbidden drop fetches external payloads or mutates the queue.
TEST_CASE(queue_drop_target_rechecks_final_drop_before_data_access) {
    DropFixture fixture;
    FileData data;
    DWORD effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
    effect = DROPEFFECT_MOVE;
    REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen({0, 0}), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    REQUIRE_EQ(data.reads, 0U);
    REQUIRE_EQ(fixture.accepts, 0U);
}

// Purpose: Queue exact decoded paths with client coordinates and release the external medium once.
// Inputs: A valid multi-path payload and a permitted copy. Outputs: Exact ordered paths and balanced ownership.
TEST_CASE(queue_drop_target_accepts_copy_with_owned_paths) {
    DropFixture fixture;
    FileData data;
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_COPY);
    REQUIRE_EQ(fixture.received,
               (std::vector<std::filesystem::path>{L"C:\\sample\\alpha.txt", L"C:\\sample\\beta.dat"}));
    REQUIRE_EQ(fixture.accepted_point.x, 30);
    REQUIRE_EQ(fixture.accepted_point.y, 40);
    REQUIRE_EQ(data.reads, 1U);
    REQUIRE_EQ(data.releases, 1U);
    REQUIRE_EQ(data.references, 1U);
    REQUIRE_TRUE(!fixture.highlighted);
    effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
}

// Purpose: Preserve host rejection when external GetData pumps messages and changes queue state.
// Inputs: A valid payload that makes the queue unavailable before acceptance. Outputs: No COPY or queue mutation.
TEST_CASE(queue_drop_target_preserves_reentrant_host_rejection) {
    DropFixture fixture;
    FileData data;
    data.on_read = [&fixture] { fixture.can_accept = false; };
    DWORD effect = DROPEFFECT_COPY;
    REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
    REQUIRE_EQ(effect, DROPEFFECT_NONE);
    REQUIRE_EQ(fixture.accepts, 1U);
    REQUIRE_TRUE(fixture.received.empty());
    REQUIRE_EQ(data.references, 1U);
    REQUIRE_EQ(data.releases, 1U);
}

// Purpose: Reject malformed or incompatible storage without retrying parsing or leaking its owner.
// Inputs: Invalid offset, wrong medium, null handle, or GetData failure. Outputs: No acceptance and exact releases.
TEST_CASE(queue_drop_target_releases_rejected_storage) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        DropFixture fixture;
        FileData data;
        if (mode == 0) {
            data.invalidate_payload();
        } else if (mode == 1) {
            data.medium_type = TYMED_NULL;
        } else if (mode == 2) {
            data.null_handle = true;
        } else {
            data.get_status = E_FAIL;
        }
        DWORD effect = DROPEFFECT_COPY;
        REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
        REQUIRE_EQ(fixture.accepts, 0U);
        REQUIRE_EQ(data.reads, 1U);
        REQUIRE_EQ(data.references, 1U);
        REQUIRE_EQ(data.releases, mode == 3 ? 0U : 1U);
    }
}

// Purpose: Reject absent and destroyed window handles before interpreting queue coordinates.
// Inputs: Null and previously destroyed private HWNDs. Outputs: No hit test, payload read or accepted effect.
TEST_CASE(queue_drop_target_rejects_invalid_window_coordinates) {
    DropFixture fixture;
    FileData data;
    HWND stale = nullptr;
    {
        TestWindow window;
        stale = window.handle;
    }
    for (const HWND window : {static_cast<HWND>(nullptr), stale}) {
        std::unique_ptr<flitzzip::app::QueueDropTarget, ReleaseTarget> target(
            new flitzzip::app::QueueDropTarget(window, fixture.callbacks()));
        DWORD effect = DROPEFFECT_COPY;
        REQUIRE_EQ(target->DragEnter(&data, 0, {30, 40}, &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
        effect = DROPEFFECT_COPY;
        REQUIRE_EQ(target->Drop(&data, 0, {30, 40}, &effect), S_OK);
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
    }
    REQUIRE_EQ(fixture.hit_tests, 0U);
    REQUIRE_EQ(data.reads, 0U);
}

// Purpose: Contain external and host exceptions at each COM drag callback.
// Inputs: Controlled query, hit-test, rendering, acceptance and feedback failures.
// Outputs: Rejected effects, no retained ownership, and independent diagnostic attempts.
TEST_CASE(queue_drop_target_contains_callback_exceptions) {
    for (unsigned mode = 0; mode < 7; ++mode) {
        DropFixture fixture;
        FileData data;
        DWORD effect = DROPEFFECT_COPY;
        REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
        data.throw_query = mode == 0;
        data.throw_get = mode == 2;
        fixture.throw_hit = mode == 1;
        fixture.throw_accept = mode == 3;
        fixture.throw_highlight = mode >= 4;
        fixture.throw_report = mode == 6;
        if (mode == 0) {
            REQUIRE_EQ(fixture.target->DragEnter(&data, 0, fixture.screen(), &effect), S_OK);
        } else if (mode == 1 || mode == 4) {
            REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
        } else if (mode == 5) {
            REQUIRE_EQ(fixture.target->DragLeave(), S_OK);
            effect = DROPEFFECT_COPY;
            fixture.throw_highlight = false;
            REQUIRE_EQ(fixture.target->DragOver(0, fixture.screen(), &effect), S_OK);
        } else {
            REQUIRE_EQ(fixture.target->Drop(&data, 0, fixture.screen(), &effect), S_OK);
        }
        REQUIRE_EQ(effect, DROPEFFECT_NONE);
        REQUIRE_EQ(data.references, 1U);
        REQUIRE_EQ(data.releases, mode == 3 ? 1U : 0U);
        REQUIRE_EQ(fixture.warnings, 1U);
        REQUIRE_TRUE(fixture.received.empty());
    }
}

// Purpose: Honor COM identity and reference ownership, including unsupported interface outputs.
// Inputs: IUnknown, IDropTarget, IDataObject and a null output pointer.
// Outputs: Stable identity, balanced references and callback destruction at final release.
TEST_CASE(queue_drop_target_com_identity_and_lifetime) {
    DropFixture fixture;
    auto lifetime = std::make_shared<int>(7);
    std::weak_ptr<int> observer = lifetime;
    auto callbacks = fixture.callbacks();
    callbacks.report_failure = [lifetime](const char*) {};
    lifetime.reset();
    auto* target = new flitzzip::app::QueueDropTarget(fixture.window.handle, std::move(callbacks));
    REQUIRE_EQ(target->QueryInterface(IID_IUnknown, nullptr), E_POINTER);
    for (const IID& iid : {IID_IUnknown, IID_IDropTarget}) {
        void* value = nullptr;
        REQUIRE_EQ(target->QueryInterface(iid, &value), S_OK);
        REQUIRE_TRUE(value == static_cast<IDropTarget*>(target));
        REQUIRE_EQ(static_cast<IDropTarget*>(value)->Release(), 1U);
    }
    void* unsupported = target;
    REQUIRE_EQ(target->QueryInterface(IID_IDataObject, &unsupported), E_NOINTERFACE);
    REQUIRE_TRUE(unsupported == nullptr);
    REQUIRE_EQ(target->AddRef(), 2U);
    REQUIRE_EQ(target->Release(), 1U);
    REQUIRE_TRUE(!observer.expired());
    REQUIRE_EQ(target->Release(), 0U);
    REQUIRE_TRUE(observer.expired());
}
