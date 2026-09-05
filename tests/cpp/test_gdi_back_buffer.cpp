#include "app/gdi_back_buffer.hpp"
#include "test_util.hpp"

#include <limits>
#include <stdexcept>

namespace {

// Purpose: Own a memory-only 32-bit test destination; inputs: none; outputs: never draws on the host desktop.
class TestSurface {
  public:
    // Purpose: Allocate a deterministic 256-square pixel target; inputs: none; outputs: throws on GDI failure.
    TestSurface() {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 256;
        info.bmiHeader.biHeight = -256;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap_ = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (dc && bitmap_) {
            original_ = SelectObject(dc, bitmap_);
        }
        if (!dc || !bitmap_ || !original_ || original_ == HGDI_ERROR) {
            release();
            throw std::runtime_error("cannot allocate memory-only GDI test surface");
        }
    }
    TestSurface(const TestSurface&) = delete;
    TestSurface& operator=(const TestSurface&) = delete;
    // Purpose: Release test resources; inputs: none; outputs: no retained GDI handles.
    ~TestSurface() {
        release();
    }
    HDC dc = nullptr;

  private:
    // Purpose: Dispose of partial or complete allocation; inputs: none; outputs: empty surface.
    void release() noexcept {
        if (dc) {
            if (original_ && original_ != HGDI_ERROR) {
                SelectObject(dc, original_);
            }
            DeleteDC(dc);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        dc = nullptr;
        bitmap_ = nullptr;
        original_ = nullptr;
    }
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
};

}  // namespace

TEST_CASE(gdi_back_buffer_reuses_surface_and_presents_changed_pixels) {
    TestSurface target;
    superzip::app::GdiBackBuffer buffer;
    for (int frame = 0; frame < 200; ++frame) {
        const auto color = RGB(frame, 20, 30);
        REQUIRE_TRUE(
            buffer.render(target.dc, 100, 80, nullptr, [color](HDC dc) { REQUIRE_TRUE(SetPixelV(dc, 9, 11, color)); }));
        REQUIRE_EQ(GetPixel(target.dc, 9, 11), color);
        REQUIRE_EQ(buffer.allocation_count(), 1U);
    }
    REQUIRE_TRUE(buffer.render(target.dc, 150, 110, nullptr,
                               [](HDC dc) { REQUIRE_TRUE(SetPixelV(dc, 149, 109, RGB(10, 220, 30))); }));
    REQUIRE_EQ(GetPixel(target.dc, 149, 109), RGB(10, 220, 30));
    REQUIRE_EQ(buffer.allocation_count(), 2U);
    buffer.reset();
    REQUIRE_TRUE(buffer.render(target.dc, 150, 110, nullptr, [](HDC) {}));
    REQUIRE_EQ(buffer.allocation_count(), 3U);
}

TEST_CASE(gdi_back_buffer_restores_state_and_font_lifetime_between_frames) {
    TestSurface target;
    superzip::app::GdiBackBuffer buffer;
    HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    REQUIRE_TRUE(font != nullptr);
    HGDIOBJ original_font = nullptr;
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, nullptr, [&](HDC dc) {
        original_font = GetCurrentObject(dc, OBJ_FONT);
        SelectObject(dc, font);
        SetViewportOrgEx(dc, 50, 50, nullptr);
        IntersectClipRect(dc, 0, 0, 2, 2);
    }));
    REQUIRE_TRUE(DeleteObject(font));
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, nullptr, [&](HDC dc) {
        REQUIRE_TRUE(GetCurrentObject(dc, OBJ_FONT) == original_font);
        POINT origin{};
        REQUIRE_TRUE(GetViewportOrgEx(dc, &origin));
        REQUIRE_EQ(origin.x, 0);
        REQUIRE_EQ(origin.y, 0);
        REQUIRE_TRUE(SetPixelV(dc, 30, 30, RGB(20, 50, 200)));
    }));
    REQUIRE_EQ(GetPixel(target.dc, 30, 30), RGB(20, 50, 200));
}

TEST_CASE(gdi_back_buffer_recovers_after_callback_exception_and_invalid_dimensions) {
    TestSurface target;
    superzip::app::GdiBackBuffer buffer;
    bool threw = false;
    try {
        (void)buffer.render(target.dc, 100, 80, nullptr, [](HDC dc) {
            SetViewportOrgEx(dc, 50, 50, nullptr);
            throw std::runtime_error("test paint failure");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    bool invoked = false;
    const auto unexpected = [&](HDC) { invoked = true; };
    REQUIRE_TRUE(!buffer.render(nullptr, 100, 80, nullptr, unexpected));
    REQUIRE_TRUE(!buffer.render(target.dc, 0, 80, nullptr, unexpected));
    REQUIRE_TRUE(!buffer.render(target.dc, -1, 80, nullptr, unexpected));
    REQUIRE_TRUE(!buffer.render(target.dc, std::numeric_limits<int>::max(), 80, nullptr, unexpected));
    REQUIRE_TRUE(!invoked);
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, nullptr,
                               [](HDC dc) { REQUIRE_TRUE(SetPixelV(dc, 3, 4, RGB(100, 120, 140))); }));
    REQUIRE_EQ(GetPixel(target.dc, 3, 4), RGB(100, 120, 140));
    REQUIRE_EQ(buffer.allocation_count(), 1U);
}

TEST_CASE(gdi_back_buffer_releases_handles_after_repeated_resize_and_destruction) {
    TestSurface target;
    const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    REQUIRE_TRUE(before > 0);
    for (int i = 0; i < 50; ++i) {
        superzip::app::GdiBackBuffer buffer;
        REQUIRE_TRUE(buffer.render(target.dc, 100, 80, nullptr, [](HDC) {}));
        REQUIRE_TRUE(buffer.render(target.dc, 110, 90, nullptr, [](HDC) {}));
    }
    REQUIRE_EQ(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), before);
}

TEST_CASE(gdi_back_buffer_invalidates_monitor_identity_and_preserves_destination_clip) {
    TestSurface target;
    superzip::app::GdiBackBuffer buffer;
    const auto monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    REQUIRE_TRUE(monitor != nullptr);
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, nullptr, [](HDC) {}));
    REQUIRE_TRUE(SetPixelV(target.dc, 3, 4, RGB(7, 8, 9)));
    const auto saved = SaveDC(target.dc);
    REQUIRE_TRUE(saved != 0);
    IntersectClipRect(target.dc, 10, 10, 20, 20);
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, monitor, [](HDC dc) {
        REQUIRE_TRUE(SetPixelV(dc, 3, 4, RGB(90, 80, 70)));
        REQUIRE_TRUE(SetPixelV(dc, 12, 13, RGB(60, 50, 40)));
    }));
    REQUIRE_TRUE(RestoreDC(target.dc, saved));
    REQUIRE_EQ(GetPixel(target.dc, 3, 4), RGB(7, 8, 9));
    REQUIRE_EQ(GetPixel(target.dc, 12, 13), RGB(60, 50, 40));
    REQUIRE_EQ(buffer.allocation_count(), 2U);
    REQUIRE_TRUE(buffer.render(target.dc, 100, 80, monitor, [](HDC) {}));
    REQUIRE_EQ(buffer.allocation_count(), 2U);
}
