#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

namespace flitzzip::app {

// Purpose: Isolate native thread admission without replacing successful execution.
// Inputs: A complete owned task; implementations must either return its owning thread or throw before starting it.
// Outputs: Transfers one joinable thread to the caller; the launcher must outlive its BackgroundJob.
class JobThreadLauncher {
  public:
    virtual ~JobThreadLauncher() = default;

    // Purpose: Select native production admission. Inputs: None. Outputs: Process-lifetime stateless launcher.
    static const JobThreadLauncher& native() {
        static const JobThreadLauncher launcher;
        return launcher;
    }

    // Purpose: Start real asynchronous work. Inputs: owned task. Outputs: Joinable thread, or startup exception.
    virtual std::thread launch(std::function<void()> task) const {
        return std::thread(std::move(task));
    }
};

// Purpose: Own one GUI job from preparation through result publication and final thread cleanup.
// Inputs: start, join and take_failure belong to one controller thread; only running may be queried concurrently.
// Outputs: Startup and worker exceptions are retained once, admission recovers, and destruction joins actual work.
class BackgroundJob final {
  public:
    enum class StartResult { Busy, Started, Failed };

    // Purpose: Select the normal native launcher. Inputs: None. Outputs: Idle owner without starting a thread.
    BackgroundJob() : BackgroundJob(JobThreadLauncher::native()) {}

    // Purpose: Select a per-instance admission boundary. Inputs: launcher outlives this owner. Outputs: Idle owner.
    explicit BackgroundJob(const JobThreadLauncher& launcher) : launcher_(launcher) {}

    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;

    // Purpose: Retain captures until their actual thread returns. Inputs: controller destruction. Outputs: Joined work.
    ~BackgroundJob() {
        join();
    }

    // Purpose: Admit preparation and all worker completion stages as one operation.
    // Inputs: prepare runs on the controller, work on the new thread, notify is nonthrowing and borrows live owners.
    // Outputs: Busy leaves callbacks untouched; failure is consumable once; notification follows terminal publication.
    template <class Prepare, class Work, class Notify> StartResult start(Prepare prepare, Work work, Notify notify) {
        static_assert(std::is_nothrow_invocable_v<Notify&>);
        if (running_.exchange(true)) {
            return StartResult::Busy;
        }
        try {
            join();
            // A controller must observe a previous failure before replacing its evidence.
            if (failure_) {
                running_.store(false);
                return StartResult::Failed;
            }
            prepare();
            thread_ = launcher_.launch([this, work = std::move(work), notify = std::move(notify)]() mutable noexcept {
                try {
                    work();
                } catch (...) {
                    failure_ = std::current_exception();
                }
                running_.store(false);
                notify();
            });
        } catch (...) {
            failure_ = std::current_exception();
            running_.store(false);
            return StartResult::Failed;
        }
        return StartResult::Started;
    }

    // Purpose: Query operation admission. Inputs: Any thread. Outputs: Includes preparation and all work stages.
    [[nodiscard]] bool running() const noexcept {
        return running_.load();
    }

    // Purpose: Wait for actual closure cleanup. Inputs: Controller thread only. Outputs: No retained joinable thread.
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Purpose: Consume a terminal exception without racing worker publication.
    // Inputs: Controller thread; active work is left alone. Outputs: Original exception once, after real cleanup joins.
    std::exception_ptr take_failure() {
        if (running()) {
            return {};
        }
        join();
        return std::exchange(failure_, {});
    }

  private:
    const JobThreadLauncher& launcher_;
    std::thread thread_;
    std::atomic_bool running_{false};
    std::exception_ptr failure_;
};

}  // namespace flitzzip::app
