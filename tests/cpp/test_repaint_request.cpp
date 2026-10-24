#include "app/repaint_request.hpp"
#include "test_util.hpp"

#include <array>
#include <atomic>
#include <thread>

namespace {

// Purpose: Own a message-only test destination without displaying or interacting with desktop windows.
// Inputs: The native STATIC window class. Outputs: An owned message queue target, destroyed on every exit.
class RepaintTestWindow {
  public:
    // Purpose: Create a message-only destination; inputs: system class; outputs: owned HWND or assertion failure.
    RepaintTestWindow() {
        window_ = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                                  nullptr);
        REQUIRE_TRUE(window_ != nullptr);
    }
    RepaintTestWindow(const RepaintTestWindow&) = delete;
    RepaintTestWindow& operator=(const RepaintTestWindow&) = delete;

    // Purpose: Destroy the private destination; inputs: owned HWND; outputs: no retained window or desktop changes.
    ~RepaintTestWindow() {
        static_cast<void>(DestroyWindow(window_));
    }

    // Purpose: Attempt real asynchronous queue admission; inputs: owned HWND; outputs: native success status.
    bool post() const noexcept {
        return PostMessageW(window_, kMessage, 0, 0) != 0;
    }

    // Purpose: Consume only this test window's notification; inputs: current owner thread; outputs: exact presence.
    bool take() const noexcept {
        MSG message{};
        return PeekMessageW(&message, window_, kMessage, kMessage, PM_REMOVE) != 0;
    }

  private:
    static constexpr UINT kMessage = WM_APP + 41;
    HWND window_ = nullptr;
};

}  // namespace

// Purpose: Prevent a refused post from permanently suppressing subsequent UI notifications.
// Inputs: One controlled platform refusal followed by real posts to an owned message-only destination.
// Outputs: Exactly one delivered notification after each refusal; ordinary duplicates stay coalesced.
TEST_CASE(repaint_request_failed_post_recovers_with_real_queue_delivery) {
    RepaintTestWindow window;
    flitzzip::app::RepaintRequest request;
    const auto post = [&]() noexcept { return window.post(); };
    for (unsigned round = 0; round < 3; ++round) {
        REQUIRE_TRUE(!request.request([]() noexcept { return false; }));
        REQUIRE_TRUE(!window.take());
        REQUIRE_TRUE(request.request(post));
        REQUIRE_TRUE(request.request(post));
        REQUIRE_TRUE(window.take());
        REQUIRE_TRUE(!window.take());
        request.acknowledge();
    }
}

// Purpose: Preserve a newer admission when an older failed post completes after UI acknowledgement.
// Inputs: A deterministic interleaving and real successful queue admission. Outputs: No lost coalescing ownership.
TEST_CASE(repaint_request_failed_old_post_preserves_newer_admission) {
    RepaintTestWindow window;
    flitzzip::app::RepaintRequest request;
    unsigned posts = 0;
    const auto post = [&]() noexcept {
        ++posts;
        return window.post();
    };
    bool newer_admitted = false;
    const bool admitted = request.request([&]() noexcept {
        request.acknowledge();
        newer_admitted = request.request(post);
        return false;
    });
    REQUIRE_TRUE(!admitted);
    REQUIRE_TRUE(newer_admitted);
    REQUIRE_EQ(posts, 1U);
    REQUIRE_TRUE(request.request(post));
    REQUIRE_EQ(posts, 1U);
    REQUIRE_TRUE(window.take());
    REQUIRE_TRUE(!window.take());
    request.acknowledge();
    REQUIRE_TRUE(request.request(post));
    REQUIRE_EQ(posts, 2U);
    REQUIRE_TRUE(window.take());
}

// Purpose: Coalesce concurrent producer bursts without dropping the next acknowledged notification.
// Inputs: Four joined producer threads and a real message-only native destination. Outputs: One post per burst.
TEST_CASE(repaint_request_concurrent_producers_coalesce_until_acknowledged) {
    RepaintTestWindow window;
    flitzzip::app::RepaintRequest request;
    for (unsigned round = 0; round < 3; ++round) {
        std::atomic_uint posts = 0;
        std::atomic_uint failed = 0;
        const auto post = [&]() noexcept {
            ++posts;
            return window.post();
        };
        std::array<std::jthread, 4> producers;
        for (auto& producer : producers) {
            producer = std::jthread([&] {
                for (unsigned repeat = 0; repeat < 32; ++repeat) {
                    if (!request.request(post))
                        ++failed;
                }
            });
        }
        for (auto& producer : producers)
            producer.join();
        REQUIRE_EQ(failed.load(), 0U);
        REQUIRE_EQ(posts.load(), 1U);
        REQUIRE_TRUE(window.take());
        REQUIRE_TRUE(!window.take());
        request.acknowledge();
    }
}
