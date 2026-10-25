#include "native_window_test_environment.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Expose an owned HWND before releasing its message pump, reproducing native startup ordering exactly.
// Inputs: Instance-local gate; no host resource exhaustion or product override. Outputs: Invisible real Windows owner.
class DelayedNativeWindow final {
  public:
    std::promise<HWND> created;
    std::future<HWND> ready = created.get_future();
    std::promise<void> released;
    std::shared_future<void> release = released.get_future().share();
    std::atomic_bool released_once{false};
    std::mutex handle_mutex;
    HWND handle = nullptr;
    std::thread thread;

    // Purpose: Publish a real window identity while its UI thread is still performing controlled initialization.
    // Inputs: Owned instance. Outputs: Window remains unresponsive until the test releases it, with a ten-second fuse.
    DelayedNativeWindow()
        : thread([this] {
              const auto window = CreateWindowExW(0, L"STATIC", L"FlitzZip startup test", WS_POPUP, 0, 0, 16, 16,
                                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
              {
                  std::lock_guard lock(handle_mutex);
                  handle = window;
              }
              created.set_value(window);
              if (window == nullptr) {
                  return;
              }
              if (release.wait_for(10s) == std::future_status::ready) {
                  MSG message{};
                  while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
                      TranslateMessage(&message);
                      DispatchMessageW(&message);
                  }
              }
              {
                  std::lock_guard lock(handle_mutex);
                  handle = nullptr;
                  if (IsWindow(window)) {
                      DestroyWindow(window);
                  }
              }
          }) {}

    // Purpose: Release initialization and join actual native teardown on every assertion path.
    // Inputs: Owned thread. Outputs: No orphan window, blocked gate or borrowed state remains.
    ~DelayedNativeWindow() {
        allow_dispatch();
        {
            std::lock_guard lock(handle_mutex);
            if (handle != nullptr) {
                PostMessageW(handle, WM_CLOSE, 0, 0);
            }
        }
        thread.join();
    }

    // Purpose: Finish the controlled initialization exactly once. Inputs: Owned gate. Outputs: UI may dispatch
    // messages.
    void allow_dispatch() {
        if (!released_once.exchange(true)) {
            released.set_value();
        }
    }

    // Purpose: Obtain only the window created by this fixture. Inputs: Creation future. Outputs: Hidden live HWND.
    HWND window() {
        REQUIRE_TRUE(ready.wait_for(3s) == std::future_status::ready);
        const auto result = ready.get();
        REQUIRE_TRUE(result != nullptr && IsWindowVisible(result) == FALSE);
        return result;
    }
};

}  // namespace

// Purpose: Distinguish the declared creation deadline from the shorter responsiveness requirement after startup.
// Inputs: Native owner deliberately held before its message loop. Outputs: Old one-second check fails, startup wait
// remains pending until release, then ordinary native input succeeds without changing its one-second deadline.
TEST_CASE(native_window_readiness_waits_for_initialization_within_creation_budget) {
    DelayedNativeWindow fixture;
    const auto window = fixture.window();
    bool old_boundary_refused = false;
    try {
        HiddenController::send(window, WM_NULL);
    } catch (const std::runtime_error& error) {
        old_boundary_refused = std::string_view(error.what()).find("win32_error=1460") != std::string_view::npos;
    }
    REQUIRE_TRUE(old_boundary_refused);
    auto startup = std::async(std::launch::async, [window] {
        HiddenController::wait_for_startup(window, std::chrono::steady_clock::now() + 10s);
    });
    REQUIRE_TRUE(startup.wait_for(1100ms) == std::future_status::timeout);
    fixture.allow_dispatch();
    REQUIRE_TRUE(startup.wait_for(3s) == std::future_status::ready);
    startup.get();
    HiddenController::send(window, WM_NULL);
}

// Purpose: Keep startup waiting bounded and prove a timed-out initialization handshake does not poison later reuse.
// Inputs: Real delayed window, expired deadline and a short positive deadline. Outputs: Both refusals, then live
// dispatch.
TEST_CASE(native_window_readiness_expiry_preserves_recovery) {
    DelayedNativeWindow fixture;
    const auto window = fixture.window();
    bool expired = false;
    try {
        HiddenController::wait_for_startup(window, std::chrono::steady_clock::now() - 1ms);
    } catch (const std::runtime_error& error) {
        expired = std::string_view(error.what()).find("deadline is expired") != std::string_view::npos;
    }
    REQUIRE_TRUE(expired);
    bool timed_out = false;
    try {
        HiddenController::wait_for_startup(window, std::chrono::steady_clock::now() + 100ms);
    } catch (const std::runtime_error& error) {
        timed_out = std::string_view(error.what()).find("win32_error=1460") != std::string_view::npos;
    }
    REQUIRE_TRUE(timed_out);
    fixture.allow_dispatch();
    HiddenController::wait_for_startup(window, std::chrono::steady_clock::now() + 10s);
    HiddenController::send(window, WM_NULL);
}
