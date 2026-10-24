#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <windows.h>

namespace flitzzip::app {

// Purpose: Isolate fallible Win32 surface acquisition and saved-state operations without replacing owned cleanup.
// Inputs: Borrowed live contexts; successful acquisitions retain the native Win32 ownership and return contracts.
// Outputs: A native result or API refusal; implementations must never delete borrowed or returned objects.
class GdiBackBufferNative {
  public:
    virtual ~GdiBackBufferNative() = default;

    // Purpose: Select the normal native boundary. Inputs: None. Outputs: Stateless process-lifetime provider.
    static const GdiBackBufferNative& native() noexcept {
        static const GdiBackBufferNative calls;
        return calls;
    }

    // Purpose: Acquire a memory context. Inputs: Borrowed destination. Outputs: Owned DC or null on refusal.
    virtual HDC create_dc(HDC target) const noexcept {
        return CreateCompatibleDC(target);
    }

    // Purpose: Acquire surface storage. Inputs: Borrowed destination and admitted dimensions. Outputs: Owned bitmap
    // or null.
    virtual HBITMAP create_bitmap(HDC target, int width, int height) const noexcept {
        return CreateCompatibleBitmap(target, width, height);
    }

    // Purpose: Bind owned storage. Inputs: Live memory DC and bitmap. Outputs: Previous bitmap or native failure.
    virtual HGDIOBJ select_bitmap(HDC dc, HBITMAP bitmap) const noexcept {
        return SelectObject(dc, bitmap);
    }

    // Purpose: Acquire native drawing state. Inputs: Live memory DC. Outputs: Saved level or zero on refusal.
    virtual int save_state(HDC dc) const noexcept {
        return SaveDC(dc);
    }

    // Purpose: Restore native drawing state. Inputs: Live memory DC and its saved level. Outputs: Native success or
    // refusal.
    virtual BOOL restore_state(HDC dc, int state) const noexcept {
        return RestoreDC(dc, state);
    }
};

// Purpose: Retain one bounded off-screen bitmap between UI-thread paints.
// Inputs: A live destination DC, physical dimensions, monitor identity, and a synchronous drawing callback.
// Outputs: Owns its GDI handles; never owns the destination or retains the callback's selected objects.
class GdiBackBuffer {
  public:
    // Purpose: Select native calls without allocating a provider. Inputs: None. Outputs: Empty surface owner.
    GdiBackBuffer() = default;

    // Purpose: Own an instance-specific external boundary through final surface cleanup.
    // Inputs: Owned provider, or null to select Win32 directly. Outputs: Empty owner; provider remains live until
    // this object's retained resources have been released.
    explicit GdiBackBuffer(std::unique_ptr<GdiBackBufferNative> calls) noexcept : calls_(std::move(calls)) {}

    GdiBackBuffer(const GdiBackBuffer&) = delete;
    GdiBackBuffer& operator=(const GdiBackBuffer&) = delete;

    // Purpose: Release the retained surface; inputs: none; outputs: deletes owned GDI objects.
    ~GdiBackBuffer() {
        reset();
    }

    // Purpose: Release the surface before display changes or destruction; inputs: none; outputs: empty cache.
    void reset() noexcept {
        if (dc_) {
            if (original_bitmap_) {
                SelectObject(dc_, original_bitmap_);
            }
            DeleteDC(dc_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        dc_ = nullptr;
        bitmap_ = nullptr;
        original_bitmap_ = nullptr;
        width_ = height_ = 0;
        monitor_ = nullptr;
    }

    // Purpose: Expose allocation work without a timing benchmark; inputs: none; outputs: lifetime surface count.
    [[nodiscard]] std::uint64_t allocation_count() const noexcept {
        return allocations_;
    }

    // Purpose: Paint and present one frame, reusing the surface only for matching geometry/device identity.
    // Inputs: target is borrowed; paint must not delete/retain the supplied DC or leave unbalanced SaveDC calls.
    // Outputs: Returns false on GDI failure for direct-paint recovery; propagates callback exceptions after cleanup.
    template <class Paint> bool render(HDC target, int width, int height, HMONITOR monitor, Paint&& paint) {
        if (!prepare(target, width, height, monitor)) {
            return false;
        }
        const int saved = calls().save_state(dc_);
        if (saved == 0) {
            reset();
            return false;
        }
        try {
            std::forward<Paint>(paint)(dc_);
        } catch (...) {
            if (!calls().restore_state(dc_, saved)) {
                reset();
            }
            throw;
        }
        if (!calls().restore_state(dc_, saved) || !BitBlt(target, 0, 0, width, height, dc_, 0, 0, SRCCOPY)) {
            reset();
            return false;
        }
        return true;
    }

  private:
    // Purpose: Resolve the owned boundary. Inputs: None. Outputs: Provider valid through resource release.
    [[nodiscard]] const GdiBackBufferNative& calls() const noexcept {
        return calls_ ? *calls_ : GdiBackBufferNative::native();
    }

    // Purpose: Allocate only when geometry/device changes; inputs: borrowed target and dimensions; outputs: ready DC.
    bool prepare(HDC target, int width, int height, HMONITOR monitor) noexcept {
        constexpr std::uint64_t max_pixels = 64ULL * 1024ULL * 1024ULL;
        if (!target || width <= 0 || height <= 0 ||
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > max_pixels) {
            return false;
        }
        if (dc_ && width == width_ && height == height_ && monitor == monitor_) {
            return true;
        }
        reset();
        dc_ = calls().create_dc(target);
        if (!dc_) {
            return false;
        }
        bitmap_ = calls().create_bitmap(target, width, height);
        if (!bitmap_) {
            reset();
            return false;
        }
        original_bitmap_ = calls().select_bitmap(dc_, bitmap_);
        if (!original_bitmap_ || original_bitmap_ == HGDI_ERROR) {
            original_bitmap_ = nullptr;
            reset();
            return false;
        }
        width_ = width;
        height_ = height;
        monitor_ = monitor;
        ++allocations_;
        return true;
    }

    std::unique_ptr<GdiBackBufferNative> calls_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    HMONITOR monitor_ = nullptr;
    std::uint64_t allocations_ = 0;
};

}  // namespace flitzzip::app
