#pragma once

#include <cstdint>
#include <utility>
#include <windows.h>

namespace superzip::app {

// Purpose: Retain one bounded off-screen bitmap between UI-thread paints.
// Inputs: A live destination DC, physical dimensions, monitor identity, and a synchronous drawing callback.
// Outputs: Owns its GDI handles; never owns the destination or retains the callback's selected objects.
class GdiBackBuffer {
  public:
    GdiBackBuffer() = default;
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
        const int saved = SaveDC(dc_);
        if (saved == 0) {
            reset();
            return false;
        }
        try {
            std::forward<Paint>(paint)(dc_);
        } catch (...) {
            if (!RestoreDC(dc_, saved)) {
                reset();
            }
            throw;
        }
        if (!RestoreDC(dc_, saved) || !BitBlt(target, 0, 0, width, height, dc_, 0, 0, SRCCOPY)) {
            reset();
            return false;
        }
        return true;
    }

  private:
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
        dc_ = CreateCompatibleDC(target);
        if (!dc_) {
            return false;
        }
        bitmap_ = CreateCompatibleBitmap(target, width, height);
        if (!bitmap_) {
            reset();
            return false;
        }
        original_bitmap_ = SelectObject(dc_, bitmap_);
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

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    HMONITOR monitor_ = nullptr;
    std::uint64_t allocations_ = 0;
};

}  // namespace superzip::app
