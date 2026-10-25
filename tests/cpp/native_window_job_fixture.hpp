#pragma once

#include "native_window_history_fixture.hpp"
#include "native_window_queue_fence.hpp"
#include "zip/zip_adapter.hpp"

namespace flitzzip_test::native_window {

enum class NativeJobResult { Success, Failure, Cancelled };

// Purpose: Control archive-job timing through existing launch admission while observing only public UI and bytes.
// Inputs: Shared History/visual fixtures. Outputs: Captured-option, completion and recovery assertions, no Shell UI.
class NativeJobSession final {
  private:
    // These owners outlive every callback and worker, including assertion-failure cleanup.
    NativeQueueFence fence_;
    flitzzip::windows_detail::HandleOwner publication_blocker_;

  public:
    HistorySession history{false};
    ReleaseOnExit release{history.ui.launcher.released};
    const std::filesystem::path archive = history.ui.fixture.output / L"FlitzZip-output.zip";

    // Purpose: Attach the instance-local ordering observer before admitting work.
    // Inputs: Created hidden window. Outputs: FIFO observations, without changing the product's notification path.
    NativeJobSession() {
        fence_.attach(history.ui.window);
    }

    // Purpose: Change real applied preferences or leave an explicitly unapplied draft.
    // Inputs: Desired summary/open flags and Apply choice. Outputs: Independent switch pixels and saved row model.
    void preferences(bool summary, bool open, bool apply = true) {
        auto& ui = history.ui;
        // Leaving Settings restores applied values; the method never infers a value from a prior abandoned draft.
        click_native_navigation(ui.window, 7);
        click_native_navigation(ui.window, 6);
        const auto form = native_settings_layout(ui.window);
        require_native_toggle(ui.window, ui.frame.dc(), form.show_operation_summary, applied_summary_);
        require_native_toggle(ui.window, ui.frame.dc(), form.open_destination_after_operation, applied_open_);
        if (summary != applied_summary_) {
            click_native_control(ui.window, form.show_operation_summary);
        }
        if (open != applied_open_) {
            click_native_control(ui.window, form.open_destination_after_operation);
        }
        require_native_toggle(ui.window, ui.frame.dc(), form.show_operation_summary, summary);
        require_native_toggle(ui.window, ui.frame.dc(), form.open_destination_after_operation, open);
        if (apply) {
            history.settings();
            applied_summary_ = summary;
            applied_open_ = open;
        }
    }

    // Purpose: Admit one actual small CPU ZIP operation and hold it before its first instruction.
    // Inputs: Success, real Windows publication refusal or pre-work cooperative cancellation.
    // Outputs: Actual started worker, with archive/source owners retained through completion.
    void start(NativeJobResult result = NativeJobResult::Success) {
        auto& ui = history.ui;
        click_native_navigation(ui.window, 0);
        if (!history.queued) {
            ControllerFixture::queue_path(ui.window, ui.fixture.input);
            history.queued = true;
        }
        ControllerFixture::show_compression(ui.window);
        click_native_control(ui.window, native_compression_layout(ui.window).destination);
        if (result == NativeJobResult::Failure) {
            publication_blocker_ = hold_native_publication_target(archive);
        }
        ControllerFixture::start(ui.window);
        ++history.jobs;
        require_native_observation([&] { return ui.launcher.attempts.load() == history.jobs; },
                                   "Native presentation job was not admitted");
        fence_.drain(ui.window);
        REQUIRE_EQ(ui.launcher.completed.load(), 0U);
        result_ = result;
        if (result == NativeJobResult::Cancelled) {
            ControllerFixture::stop(ui.window);
        }
    }

    // Purpose: Complete the held job and observe its actual UI notification before asserting absence or ordering.
    // Inputs: Already admitted worker. Outputs: Literal History model, preserved failure bytes or decoded source.
    void complete() {
        auto& ui = history.ui;
        release.release();
        require_native_observation([&] { return ui.launcher.completed.load() == history.jobs; },
                                   "Native presentation worker did not finish");
        fence_.drain(ui.window);
        const bool success = result_ == NativeJobResult::Success;
        history.rows.push_back(
            {L"Compress",
             success ? L"FlitzZip-output.zip"
                     : (result_ == NativeJobResult::Cancelled ? L"Compress stopped" : L"Compress failure"),
             success});
        if (success) {
            const auto restored = ui.fixture.root / L"restored";
            flitzzip::extract_zip(archive, restored, false);
            REQUIRE_EQ(read_native_fixture(restored / L"source.txt"), "controller ownership fixture\n");
        } else if (result_ == NativeJobResult::Failure) {
            REQUIRE_EQ(read_native_fixture(archive), "preserve existing destination\n");
        } else {
            REQUIRE_TRUE(std::filesystem::is_empty(ui.fixture.output));
        }
        if (result_ != NativeJobResult::Cancelled) {
            history.artifacts.emplace_back(archive, read_native_fixture(archive));
        }
    }

    // Purpose: Prove a summary selected the actual result and reset previously incompatible filters.
    // Inputs: Completed job and independent row model. Outputs: History page, literal filters, row and selection.
    void require_summary(std::size_t dispatches = 1) {
        auto& ui = history.ui;
        require_native_navigation_selection(ui.window, ui.frame.dc(), 4);
        history.operation = 0;
        history.status = 0;
        const auto form = history.layout();
        ui.require_field(form.operation_filter, L"All operations");
        ui.require_field(form.status_filter, L"All");
        const auto index = history.rows.size() - 1;
        history.require_row(static_cast<int>(index), index);
        history.require_selected(static_cast<int>(index));
        require_native_log_count("Operation summary shown: Compress", dispatches);
    }

    // Purpose: Observe the captured destination request without starting Explorer or relying on timing.
    // Inputs: Expected directory and dispatch count. Outputs: Exact full-path marker and no alternate Shell result.
    void require_destination(const std::filesystem::path& path, std::size_t count) const {
        const auto encoded = path.u8string();
        std::string expected;
        for (const auto byte : encoded) {
            // Windows fixture paths contain no quotes or control bytes; persistent logs escape native separators.
            if (byte == u8'\\') {
                expected.push_back('\\');
            }
            expected.push_back(static_cast<char>(byte));
        }
        require_native_log_count("Destination folder open suppressed (GUI smoke): " + expected, count);
        require_native_log_count("Destination folder open suppressed (GUI smoke): ", count);
        require_native_log_count("Destination folder opened:", 0);
        require_native_log_count("Open destination failed:", 0);
    }

    // Purpose: Verify one-shot consumption after further ordinary input and queued paint processing.
    // Inputs: Completed job. Outputs: Real benign input followed by an observation-only fence.
    void more_input() const {
        HiddenController::send(history.ui.window, WM_MOUSELEAVE);
        settle();
    }

    // Purpose: Observe real completion dispatch after another shared-fixture job.
    // Inputs: A completed launcher count. Outputs: FIFO acknowledgement without an extra product input.
    void settle() const {
        fence_.drain(history.ui.window);
    }

    // Purpose: Join all work and verify that observation hooks, artifacts and shared fixture state clean up.
    // Inputs: Completed session. Outputs: Preserved bytes, exact worker counts and removed per-window observation.
    void finish() {
        history.finish();
        fence_.require_detached();
    }

  private:
    bool applied_summary_ = false;
    bool applied_open_ = false;
    NativeJobResult result_ = NativeJobResult::Success;
};

}  // namespace flitzzip_test::native_window
