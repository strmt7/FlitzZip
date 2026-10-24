#include "app/background_job.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <system_error>
#include <windows.h>

namespace {

class ControlledLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    mutable unsigned attempts = 0;
    mutable std::thread::id launched_id;
    bool refuse = true;

    // Purpose: Refuse admission without exhausting host threads, then use real native execution.
    // Inputs: Owned production closure and per-instance refusal. Outputs: Native owner or specified system error.
    std::thread launch(std::function<void()> task) const override {
        ++attempts;
        if (refuse) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        auto thread = JobThreadLauncher::launch(std::move(task));
        launched_id = thread.get_id();
        return thread;
    }
};

// Purpose: Own a private notification target without opening a desktop GUI.
// Inputs: Native STATIC class. Outputs: Message-only window destroyed after every joined worker.
class CompletionWindow final {
  public:
    // Purpose: Acquire the notification destination. Inputs: Native class. Outputs: Owned HWND or assertion.
    CompletionWindow() {
        window_ = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                                  nullptr);
        REQUIRE_TRUE(window_ != nullptr);
    }
    CompletionWindow(const CompletionWindow&) = delete;
    CompletionWindow& operator=(const CompletionWindow&) = delete;

    // Purpose: Release the destination. Inputs: Joined borrowers. Outputs: No retained window.
    ~CompletionWindow() {
        static_cast<void>(DestroyWindow(window_));
    }

    // Purpose: Notify the controller through native admission. Inputs: Live target. Outputs: Actual post result.
    bool post() const noexcept {
        return PostMessageW(window_, WM_APP + 43, 0, 0) != 0;
    }

    // Purpose: Consume one terminal message. Inputs: Controller thread. Outputs: Exact delivery presence.
    bool take() const noexcept {
        MSG message{};
        return PeekMessageW(&message, window_, WM_APP + 43, WM_APP + 43, PM_REMOVE) != 0;
    }

  private:
    HWND window_ = nullptr;
};

// Purpose: Check an original preparation failure, independent of exception formatting in the owner.
// Inputs: Retained exception. Outputs: Exact original runtime_error or an assertion failure.
void require_preparation_failure(std::exception_ptr failure) {
    REQUIRE_TRUE(failure != nullptr);
    bool matched = false;
    try {
        std::rethrow_exception(failure);
    } catch (const std::runtime_error& error) {
        matched = std::string(error.what()) == "controlled preparation failure";
    }
    REQUIRE_TRUE(matched);
}

}  // namespace

// Purpose: Restore admission after fallible preparation and preserve the original error exactly once.
// Inputs: Controlled preparation refusal followed by a real worker and native completion message.
// Outputs: No task on refusal; valid reuse runs on another thread and delivers exactly one completion.
TEST_CASE(background_job_preparation_failure_recovers_without_launch) {
    CompletionWindow window;
    ControlledLauncher launcher;
    launcher.refuse = false;
    flitzzip::app::BackgroundJob worker(launcher);
    unsigned work_calls = 0;
    std::atomic_bool posted = false;
    const auto notify = [&]() noexcept { posted.store(window.post()); };
    const auto failed =
        worker.start([] { throw std::runtime_error("controlled preparation failure"); }, [&] { ++work_calls; }, notify);
    REQUIRE_TRUE(failed == flitzzip::app::BackgroundJob::StartResult::Failed);
    REQUIRE_TRUE(!worker.running());
    REQUIRE_EQ(launcher.attempts, 0U);
    REQUIRE_EQ(work_calls, 0U);
    REQUIRE_TRUE(!window.take());
    // A new operation must not overwrite an unobserved failure.
    REQUIRE_TRUE(worker.start([] {}, [] {}, notify) == flitzzip::app::BackgroundJob::StartResult::Failed);
    require_preparation_failure(worker.take_failure());
    REQUIRE_TRUE(!worker.take_failure());
    REQUIRE_TRUE(worker.start([] {}, [&] { ++work_calls; }, notify) ==
                 flitzzip::app::BackgroundJob::StartResult::Started);
    worker.join();
    REQUIRE_EQ(work_calls, 1U);
    REQUIRE_TRUE(launcher.launched_id != std::this_thread::get_id());
    REQUIRE_TRUE(posted.load());
    REQUIRE_TRUE(window.take());
    REQUIRE_TRUE(!window.take());
    REQUIRE_TRUE(!worker.take_failure());
}

// Purpose: Recover from native thread-admission refusal without fabricating a successful operation.
// Inputs: One per-instance launch failure followed by a real successful launch. Outputs: Original error and clean
// reuse.
TEST_CASE(background_job_thread_admission_failure_preserves_error_and_reuses_owner) {
    ControlledLauncher launcher;
    flitzzip::app::BackgroundJob worker(launcher);
    unsigned prepared = 0;
    unsigned executed = 0;
    std::atomic_uint notified = 0;
    const auto notify = [&]() noexcept { ++notified; };
    REQUIRE_TRUE(worker.start([&] { ++prepared; }, [&] { ++executed; }, notify) ==
                 flitzzip::app::BackgroundJob::StartResult::Failed);
    REQUIRE_TRUE(!worker.running());
    REQUIRE_EQ(prepared, 1U);
    REQUIRE_EQ(executed, 0U);
    bool matched = false;
    const auto failure = worker.take_failure();
    REQUIRE_TRUE(failure != nullptr);
    try {
        std::rethrow_exception(failure);
    } catch (const std::system_error& error) {
        matched = error.code() == std::errc::resource_unavailable_try_again;
    }
    REQUIRE_TRUE(matched);
    REQUIRE_EQ(notified.load(), 0U);
    launcher.refuse = false;
    REQUIRE_TRUE(worker.start([&] { ++prepared; }, [&] { ++executed; }, notify) ==
                 flitzzip::app::BackgroundJob::StartResult::Started);
    worker.join();
    REQUIRE_EQ(prepared, 2U);
    REQUIRE_EQ(executed, 1U);
    REQUIRE_EQ(notified.load(), 1U);
    REQUIRE_TRUE(!worker.take_failure());
}

// Purpose: Contain failures from work and later result publication instead of escaping the native thread entry.
// Inputs: Standard and nonstandard exceptions at distinct worker stages, then a successful real worker.
// Outputs: Exact exception identity, terminal notification after idle publication, and no lost admission.
TEST_CASE(background_job_contains_all_worker_stages_and_notifies_terminal_state) {
    CompletionWindow window;
    flitzzip::app::BackgroundJob worker;
    for (unsigned stage = 0; stage < 3; ++stage) {
        std::atomic_uint reached = 0;
        std::atomic_bool idle_at_notification = false;
        std::atomic_bool posted = false;
        const auto result = worker.start([] {},
                                         [&] {
                                             ++reached;
                                             if (stage == 0) {
                                                 throw std::runtime_error("controlled preparation failure");
                                             }
                                             ++reached;
                                             if (stage == 1) {
                                                 throw 73;
                                             }
                                         },
                                         [&]() noexcept {
                                             idle_at_notification.store(!worker.running());
                                             posted.store(window.post());
                                         });
        REQUIRE_TRUE(result == flitzzip::app::BackgroundJob::StartResult::Started);
        worker.join();
        REQUIRE_TRUE(idle_at_notification.load());
        REQUIRE_TRUE(posted.load());
        REQUIRE_TRUE(window.take());
        REQUIRE_TRUE(!window.take());
        const auto failure = worker.take_failure();
        if (stage == 0) {
            REQUIRE_EQ(reached.load(), 1U);
            require_preparation_failure(failure);
        } else if (stage == 1) {
            REQUIRE_EQ(reached.load(), 2U);
            REQUIRE_TRUE(failure != nullptr);
            bool matched = false;
            try {
                std::rethrow_exception(failure);
            } catch (int value) {
                matched = value == 73;
            }
            REQUIRE_TRUE(matched);
        } else {
            REQUIRE_EQ(reached.load(), 2U);
            REQUIRE_TRUE(!failure);
        }
    }
}

// Purpose: Reject overlapping work while retaining borrowed owners through actual notification cleanup.
// Inputs: Bounded promise-controlled work and notification, without sleeps or host-resource exhaustion.
// Outputs: No second preparation; running covers work, and join waits beyond terminal publication.
TEST_CASE(background_job_busy_admission_and_join_cover_actual_thread_lifetime) {
    using namespace std::chrono_literals;
    std::promise<void> work_entered;
    std::promise<void> release_work;
    auto released = release_work.get_future();
    std::promise<void> notify_entered;
    std::promise<void> release_notify;
    auto notification_released = release_notify.get_future();
    std::atomic_bool notify_returned = false;
    std::atomic_bool work_released = false;
    flitzzip::app::BackgroundJob worker;
    const auto result = worker.start([] {},
                                     [&] {
                                         work_entered.set_value();
                                         work_released.store(released.wait_for(5s) == std::future_status::ready);
                                     },
                                     [&]() noexcept {
                                         notify_entered.set_value();
                                         static_cast<void>(notification_released.wait_for(5s));
                                         notify_returned.store(true);
                                     });
    const bool entered = work_entered.get_future().wait_for(2s) == std::future_status::ready;
    const bool busy = worker.running();
    const bool absent_failure = !worker.take_failure();
    unsigned overlapping_preparation = 0;
    const auto overlap = worker.start([&] { ++overlapping_preparation; }, [] {}, []() noexcept {});
    release_work.set_value();
    const bool terminal = notify_entered.get_future().wait_for(2s) == std::future_status::ready;
    const bool terminal_idle = !worker.running();
    const bool cleanup_pending = !notify_returned.load();
    // The controller can observe terminal admission before the thread has finished using its captures.
    release_notify.set_value();
    worker.join();
    REQUIRE_TRUE(result == flitzzip::app::BackgroundJob::StartResult::Started);
    REQUIRE_TRUE(entered && busy && absent_failure && terminal && terminal_idle && cleanup_pending);
    REQUIRE_TRUE(overlap == flitzzip::app::BackgroundJob::StartResult::Busy);
    REQUIRE_EQ(overlapping_preparation, 0U);
    REQUIRE_TRUE(work_released.load());
    REQUIRE_TRUE(notify_returned.load());
    REQUIRE_TRUE(!worker.take_failure());
}
