#pragma once

#include "native_window_test_environment.hpp"

namespace flitzzip_test::native_window {

// Purpose: Observe FIFO dispatch after actual worker notification without forcing product repaint or completion.
// Inputs: One owned hidden window in this process. Outputs: Instance-local queue fence, removed at WM_NCDESTROY.
// This context must outlive the controller; no global hooks or application state are replaced.
class NativeQueueFence final {
  public:
    // Purpose: Attach an observation-only subclass after initialization.
    // Inputs: Live owned HWND, without another test subclass. Outputs: Published callback context or test failure.
    void attach(HWND window) {
        REQUIRE_TRUE(event_ != nullptr && message_ != 0);
        DWORD process = 0;
        REQUIRE_TRUE(GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId());
        REQUIRE_TRUE(GetPropW(window, kProperty) == nullptr);
        original_ = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
        REQUIRE_TRUE(original_ != nullptr);
        REQUIRE_TRUE(SetPropW(window, kProperty, this) != FALSE);
        ready_.store(true, std::memory_order_release);
        const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dispatch));
        if (previous == 0) {
            RemovePropW(window, kProperty);
            throw std::runtime_error("Cannot attach native queue observation");
        }
        REQUIRE_TRUE(reinterpret_cast<WNDPROC>(previous) == original_);
    }

    // Purpose: Await earlier ordinary posted messages using their FIFO ordering.
    // Inputs: Live owned window, after observing the launcher's actual completion counter when testing completion.
    // Outputs: Bounded event acknowledgement; no production message or work is injected.
    void drain(HWND window) const {
        REQUIRE_TRUE(PostMessageW(window, message_, 0, 0) != FALSE);
        REQUIRE_TRUE(WaitForSingleObject(event_.get(), 3000) == WAIT_OBJECT_0);
        REQUIRE_EQ(error_.load(), ERROR_SUCCESS);
    }

    // Purpose: Verify observation cleanup after joining the controller.
    // Inputs: Already destroyed window. Outputs: Confirmed removal on its UI thread and no native cleanup error.
    void require_detached() const {
        REQUIRE_TRUE(detached_.load());
        REQUIRE_EQ(error_.load(), ERROR_SUCCESS);
    }

  private:
    // Purpose: Observe only the fence and forward every actual product event to the original procedure.
    // Inputs: Native callback on the owned UI thread. Outputs: Fence signal or unchanged original dispatch result.
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = static_cast<NativeQueueFence*>(GetPropW(window, kProperty));
        if (!self || !self->ready_.load(std::memory_order_acquire)) {
            std::terminate();
        }
        if (message == self->message_) {
            if (!SetEvent(self->event_.get())) {
                self->error_.store(GetLastError());
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            if (SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(self->original_)) == 0 ||
                RemovePropW(window, kProperty) != self) {
                self->error_.store(ERROR_INVALID_FUNCTION);
            }
            self->detached_.store(true);
        }
        return CallWindowProcW(self->original_, window, message, wparam, lparam);
    }

    static constexpr const wchar_t* kProperty = L"FlitzZipTest.QueueFence";
    flitzzip::windows_detail::HandleOwner event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    const UINT message_ = RegisterWindowMessageW(L"FlitzZipTest.QueueFence.Dispatch");
    WNDPROC original_ = nullptr;
    std::atomic_bool ready_{false};
    std::atomic_bool detached_{false};
    std::atomic<DWORD> error_{ERROR_SUCCESS};
};

}  // namespace flitzzip_test::native_window
