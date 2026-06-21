#include "app/main_window_impl.hpp"

#include "core/resource_usage.hpp"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif

namespace superzip::app {
namespace {

// Purpose: Build one English PDH LogicalDisk counter path.
// Inputs: `instance` is a fixed-drive instance such as `C:` and `counter` is the PDH counter name.
// Outputs: Returns a fully qualified counter path.
std::wstring logical_disk_counter_path(std::wstring_view instance, std::wstring_view counter) {
    std::wstring path = L"\\LogicalDisk(";
    path.append(instance);
    path += L")\\";
    path.append(counter);
    return path;
}

// Purpose: Read a formatted double value from a PDH counter.
// Inputs: `counter` is an initialized PDH counter and `value` receives the formatted sample.
// Outputs: Returns true only when PDH reports valid counter data.
bool read_pdh_double(PDH_HCOUNTER counter, double& value) {
    PDH_FMT_COUNTERVALUE formatted{};
    DWORD value_type = 0;
    const PDH_STATUS status = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &value_type, &formatted);
    if (status != ERROR_SUCCESS || formatted.CStatus != PDH_CSTATUS_VALID_DATA) {
        return false;
    }
    value = formatted.doubleValue;
    return true;
}

}  // namespace

// Purpose: Refresh AMD HIP availability and device identity for the UI.
// Inputs: None; queries the configured GPU backend.
// Outputs: Updates synchronized GPU status fields.
void MainWindow::refresh_gpu_status() {
    const auto info = query_gpu_info();
    std::lock_guard lock(mutex_);
    state_.gpu_status = info.status;
    state_.gpu_runtime_name = info.runtime_name;
    state_.gpu_device_name = info.device_name;
    state_.gpu_arch = info.gcn_arch;
}

// Purpose: Initialize optional Windows performance counters for live monitoring.
// Inputs: None; uses the current process and Windows PDH provider.
// Outputs: Opens best-effort GPU engine counters without failing app startup.
void MainWindow::initialize_performance_monitor() {
    if (gpu_query_ != nullptr) {
        return;
    }
    if (PdhOpenQueryW(nullptr, 0, &gpu_query_) != ERROR_SUCCESS) {
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return;
    }
    const PDH_STATUS status =
        PdhAddEnglishCounterW(gpu_query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_counter_);
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return;
    }
    PdhCollectQueryData(gpu_query_);
}

// Purpose: Release live performance counter resources.
// Inputs: None.
// Outputs: Closes the PDH query and clears owned counter handles.
void MainWindow::shutdown_performance_monitor() {
    if (gpu_query_ != nullptr) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
    }
    reset_disk_performance_monitor();
}

// Purpose: Re-arm the live performance sampling timer at the selected interval.
// Inputs: `seconds` is normalized to one of the supported 1, 3, 5, or 10 second ranges.
// Outputs: Replaces the existing timer interval for the main window.
void MainWindow::reset_performance_timer(int seconds) {
    if (hwnd_ == nullptr) {
        return;
    }
    const auto interval = static_cast<UINT>(normalize_performance_update_seconds(seconds) * 1000);
    KillTimer(hwnd_, kPerformanceTimer);
    SetTimer(hwnd_, kPerformanceTimer, interval, nullptr);
}

// Purpose: Sample total Windows GPU engine utilization when PDH exposes it.
// Inputs: None; uses initialized PDH wildcard counters.
// Outputs: Returns total system GPU percentage or a negative value when unavailable.
double MainWindow::sample_gpu_utilization() {
    if (gpu_query_ == nullptr || gpu_counter_ == nullptr) {
        return -1.0;
    }
    if (PdhCollectQueryData(gpu_query_) != ERROR_SUCCESS) {
        return -1.0;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0 || item_count == 0) {
        return -1.0;
    }
    std::vector<std::byte> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return -1.0;
    }

    double system_total = 0.0;
    for (DWORD index = 0; index < item_count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.szName == nullptr) {
            continue;
        }
        const double value = std::max(0.0, item.FmtValue.doubleValue);
        system_total += value;
    }
    return std::clamp(system_total, 0.0, 100.0);
}

// Purpose: Sample SuperZip process CPU use since the previous monitor tick.
// Inputs: `elapsed_seconds` is the interval from the previous sample.
// Outputs: Returns total-system-capacity CPU percentage and updates previous process FILETIME state.
double MainWindow::sample_process_cpu_percent(double elapsed_seconds) {
    double cpu_percent = 0.0;
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        return cpu_percent;
    }
    if (elapsed_seconds > 0.0) {
        const std::uint64_t previous_ticks =
            filetime_ticks(last_process_kernel_time_) + filetime_ticks(last_process_user_time_);
        const std::uint64_t current_ticks = filetime_ticks(kernel_time) + filetime_ticks(user_time);
        const auto logical_processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        if (current_ticks >= previous_ticks) {
            cpu_percent = (static_cast<double>(current_ticks - previous_ticks) /
                           (elapsed_seconds * 10000000.0 * static_cast<double>(logical_processors))) *
                          100.0;
            cpu_percent = std::clamp(cpu_percent, 0.0, 100.0);
        }
    }
    last_process_kernel_time_ = kernel_time;
    last_process_user_time_ = user_time;
    return cpu_percent;
}

// Purpose: Sample total system CPU use since the previous monitor tick.
// Inputs: `elapsed_seconds` is the interval from the previous sample.
// Outputs: Returns logical-processor-normalized CPU percentage and updates previous system FILETIME state.
double MainWindow::sample_system_cpu_percent(double elapsed_seconds) {
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return 0.0;
    }
    double cpu_percent = 0.0;
    if (elapsed_seconds > 0.0) {
        const std::uint64_t previous_idle = filetime_ticks(last_system_idle_time_);
        const std::uint64_t previous_kernel = filetime_ticks(last_system_kernel_time_);
        const std::uint64_t previous_user = filetime_ticks(last_system_user_time_);
        const std::uint64_t current_idle = filetime_ticks(idle_time);
        const std::uint64_t current_kernel = filetime_ticks(kernel_time);
        const std::uint64_t current_user = filetime_ticks(user_time);
        const std::uint64_t previous_total = previous_kernel + previous_user;
        const std::uint64_t current_total = current_kernel + current_user;
        if (current_total >= previous_total && current_idle >= previous_idle) {
            const auto total_delta = current_total - previous_total;
            const auto idle_delta = current_idle - previous_idle;
            if (total_delta > 0U) {
                const auto busy_delta = total_delta > idle_delta ? total_delta - idle_delta : 0U;
                cpu_percent = (static_cast<double>(busy_delta) / static_cast<double>(total_delta)) * 100.0;
            }
        }
    }
    last_system_idle_time_ = idle_time;
    last_system_kernel_time_ = kernel_time;
    last_system_user_time_ = user_time;
    return std::clamp(cpu_percent, 0.0, 100.0);
}

// Purpose: Sample Windows GPU dedicated memory assigned to the SuperZip process.
// Inputs: None; uses current-process PDH GPU Process Memory counters.
// Outputs: Returns dedicated GPU memory bytes or zero when Windows does not expose the counter.
std::uint64_t MainWindow::sample_process_dedicated_vram_bytes() const {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) {
        return 0U;
    }
    struct QueryGuard {
        PDH_HQUERY query = nullptr;
        ~QueryGuard() {
            if (query != nullptr) {
                PdhCloseQuery(query);
            }
        }
    } guard{query};

    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &counter) != ERROR_SUCCESS ||
        counter == nullptr) {
        return 0U;
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return 0U;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0U || item_count == 0U) {
        return 0U;
    }
    std::vector<std::byte> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return 0U;
    }

    const std::wstring pid_marker = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::uint64_t total = 0U;
    for (DWORD index = 0; index < item_count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.szName == nullptr ||
            std::wstring_view(item.szName).find(pid_marker) == std::wstring_view::npos) {
            continue;
        }
        if (item.FmtValue.largeValue > 0) {
            total += static_cast<std::uint64_t>(item.FmtValue.largeValue);
        }
    }
    return total;
}

// Purpose: Sample Windows-visible total dedicated GPU memory usage.
// Inputs: None; reads PDH GPU Adapter Memory wildcard counters for dedicated usage.
// Outputs: Returns the summed dedicated usage bytes, or zero when the counter is unavailable.
std::uint64_t MainWindow::sample_total_dedicated_vram_used_bytes() const {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) {
        return 0U;
    }
    struct QueryGuard {
        PDH_HQUERY query = nullptr;
        ~QueryGuard() {
            if (query != nullptr) {
                PdhCloseQuery(query);
            }
        }
    } guard{query};

    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &counter) != ERROR_SUCCESS ||
        counter == nullptr) {
        return 0U;
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return 0U;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0U || item_count == 0U) {
        return 0U;
    }
    std::vector<std::byte> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return 0U;
    }

    std::uint64_t total = 0U;
    for (DWORD index = 0; index < item_count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.szName == nullptr) {
            continue;
        }
        if (item.FmtValue.largeValue > 0) {
            total += static_cast<std::uint64_t>(item.FmtValue.largeValue);
        }
    }
    return total;
}

// Purpose: Release selected-drive I/O performance counters after a drive-selection change.
// Inputs: None.
// Outputs: Closes disk PDH handles so the next sample binds to the selected fixed drive.
void MainWindow::reset_disk_performance_monitor() {
    if (disk_query_ != nullptr) {
        PdhCloseQuery(disk_query_);
    }
    disk_query_ = nullptr;
    disk_busy_counter_ = nullptr;
    disk_read_counter_ = nullptr;
    disk_write_counter_ = nullptr;
    disk_counter_instance_.clear();
}

// Purpose: Sample total selected fixed-drive activity through Windows performance counters.
// Inputs: `selected_drive_index` identifies the user-selected fixed-drive row.
// Outputs: Returns drive busy percent plus total read/write byte rates, or unavailable values on counter failure.
MainWindow::ProcessIoRates MainWindow::sample_selected_drive_io(int selected_drive_index) {
    ProcessIoRates rates;
    const auto drives = fixed_io_drive_options();
    if (drives.empty()) {
        return rates;
    }
    const std::wstring instance = drives[static_cast<std::size_t>(normalize_io_drive_index(selected_drive_index))];
    if (disk_query_ == nullptr || disk_counter_instance_ != instance) {
        reset_disk_performance_monitor();
        disk_counter_instance_ = instance;
        if (PdhOpenQueryW(nullptr, 0, &disk_query_) != ERROR_SUCCESS || disk_query_ == nullptr) {
            reset_disk_performance_monitor();
            return rates;
        }
        const auto busy_path = logical_disk_counter_path(instance, L"% Disk Time");
        const auto read_path = logical_disk_counter_path(instance, L"Disk Read Bytes/sec");
        const auto write_path = logical_disk_counter_path(instance, L"Disk Write Bytes/sec");
        if (PdhAddEnglishCounterW(disk_query_, busy_path.c_str(), 0, &disk_busy_counter_) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(disk_query_, read_path.c_str(), 0, &disk_read_counter_) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(disk_query_, write_path.c_str(), 0, &disk_write_counter_) != ERROR_SUCCESS) {
            reset_disk_performance_monitor();
            return rates;
        }
        (void)PdhCollectQueryData(disk_query_);
    }

    if (PdhCollectQueryData(disk_query_) != ERROR_SUCCESS) {
        reset_disk_performance_monitor();
        return rates;
    }
    double busy = 0.0;
    double read = 0.0;
    double write = 0.0;
    const bool busy_ok = disk_busy_counter_ != nullptr && read_pdh_double(disk_busy_counter_, busy);
    const bool read_ok = disk_read_counter_ != nullptr && read_pdh_double(disk_read_counter_, read);
    const bool write_ok = disk_write_counter_ != nullptr && read_pdh_double(disk_write_counter_, write);
    if (busy_ok || read_ok || write_ok) {
        rates.available = true;
        rates.busy_percent = busy_ok ? std::clamp(busy, 0.0, 100.0) : 0.0;
        rates.read_bytes_per_second = read_ok ? std::max(0.0, read) : 0.0;
        rates.write_bytes_per_second = write_ok ? std::max(0.0, write) : 0.0;
    }
    return rates;
}

// Purpose: Refresh cached HIP VRAM and visible GPU identity at a bounded cadence.
// Inputs: `now` is the current steady-clock timestamp.
// Outputs: Updates cached VRAM fields and GPU status text when the throttle permits a HIP query.
void MainWindow::refresh_gpu_memory_cache(std::chrono::steady_clock::time_point now) {
    if (last_gpu_memory_sample_time_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gpu_memory_sample_time_).count() <
            kGpuMemorySampleMs) {
        return;
    }
    const auto info = query_gpu_info();
    cached_vram_total_bytes_ = info.vram_total_bytes;
    cached_vram_free_bytes_ = info.vram_free_bytes;
    last_gpu_memory_sample_time_ = now;
    std::lock_guard lock(mutex_);
    state_.gpu_status = info.status;
    state_.gpu_runtime_name = info.runtime_name;
    state_.gpu_device_name = info.device_name;
    state_.gpu_arch = info.gcn_arch;
}

// Purpose: Publish a completed performance sample into current UI state and graph history.
// Inputs: `sample` is the counter set and `now` is the sample timestamp.
// Outputs: Updates visible performance state, ring-buffer graph history, and last-sample time.
void MainWindow::publish_performance_sample(const PerformanceMonitorSample& sample,
                                            std::chrono::steady_clock::time_point now) {
    {
        std::lock_guard lock(mutex_);
        state_.performance = sample;
    }
    performance_history_[performance_history_next_] = sample;
    performance_history_next_ = (performance_history_next_ + 1U) % performance_history_.size();
    performance_history_count_ = std::min(performance_history_count_ + 1U, performance_history_.size());
    last_performance_sample_time_ = now;
}

// Purpose: Collect one live performance sample for the System page.
// Inputs: None; reads process, memory, I/O, optional PDH, and throttled HIP memory state.
// Outputs: Updates `state_.performance` with CPU, RAM, I/O, GPU utilization, and VRAM values.
void MainWindow::update_performance_sample() {
    const auto now = std::chrono::steady_clock::now();
    double elapsed_seconds = 0.0;
    if (last_performance_sample_time_ != std::chrono::steady_clock::time_point{}) {
        elapsed_seconds = std::chrono::duration<double>(now - last_performance_sample_time_).count();
    }

    const double cpu_percent = sample_system_cpu_percent(elapsed_seconds);
    const double process_cpu_percent = sample_process_cpu_percent(elapsed_seconds);
    int io_drive_index = 0;
    {
        std::lock_guard lock(mutex_);
        io_drive_index = state_.io_drive_index;
    }
    const auto io_rates = sample_selected_drive_io(io_drive_index);
    const auto private_bytes = sample_private_memory_bytes();
    const auto system_memory = sample_system_memory_usage();
    refresh_gpu_memory_cache(now);
    const double gpu_percent = sample_gpu_utilization();
    const auto process_dedicated_vram_bytes = sample_process_dedicated_vram_bytes();
    const auto adapter_dedicated_vram_used_bytes = sample_total_dedicated_vram_used_bytes();
    const auto vram_usage = reconcile_vram_usage(cached_vram_total_bytes_, cached_vram_free_bytes_,
                                                 adapter_dedicated_vram_used_bytes, process_dedicated_vram_bytes);

    PerformanceMonitorSample sample;
    sample.live = true;
    sample.gpu_utilization_available = gpu_percent >= 0.0;
    sample.cpu_percent = cpu_percent;
    sample.process_cpu_percent = process_cpu_percent;
    sample.gpu_utilization_percent = sample.gpu_utilization_available ? gpu_percent : 0.0;
    sample.system_memory_percent = system_memory.percent;
    sample.io_busy_percent = io_rates.available ? io_rates.busy_percent : 0.0;
    sample.io_read_bytes_per_second = std::max(0.0, io_rates.read_bytes_per_second);
    sample.io_write_bytes_per_second = std::max(0.0, io_rates.write_bytes_per_second);
    sample.private_bytes = private_bytes;
    sample.system_memory_total_bytes = system_memory.total_bytes;
    sample.system_memory_used_bytes = system_memory.used_bytes;
    sample.vram_total_bytes = vram_usage.total_capacity_bytes;
    sample.vram_free_bytes = sample.vram_total_bytes >= vram_usage.total_used_bytes
                                 ? sample.vram_total_bytes - vram_usage.total_used_bytes
                                 : 0U;
    sample.process_dedicated_vram_bytes = vram_usage.process_dedicated_bytes;
    publish_performance_sample(sample, now);
}

// Purpose: Start the tab transition animation.
// Inputs: `from` is the outgoing page and `to` is the incoming page.
// Outputs: Records transition state and arms the animation timer.
void MainWindow::start_page_transition(Page from, Page to) {
    transition_from_page_ = from;
    transition_to_page_ = to;
    page_transition_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

// Purpose: Start a short visual animation for a toggle state change.
// Inputs: `id` identifies the toggle and `from`/`to` are the logical states.
// Outputs: Records transition state and arms the animation timer.
void MainWindow::start_toggle_animation(ToggleId id, bool from, bool to) {
    transition_toggle_ = id;
    transition_toggle_from_ = from;
    transition_toggle_to_ = to;
    toggle_transition_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

// Purpose: Start a bounded non-blocking command-button release animation.
// Inputs: `point` is the client-coordinate release position after a primary mouse click.
// Outputs: Records the release pulse origin and arms the animation timer.
void MainWindow::start_button_release_animation(POINT point) {
    button_release_point_ = point;
    button_release_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

// Purpose: Report normalized tab transition progress.
// Inputs: None; reads the transition timestamp.
// Outputs: Returns 1.0 when inactive or a 0.0-1.0 progress fraction.
double MainWindow::page_transition_progress() const {
    if (page_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return 1.0;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - page_transition_start_)
            .count();
    return std::clamp(static_cast<double>(elapsed) / static_cast<double>(kPageTransitionMs), 0.0, 1.0);
}

// Purpose: Return a toggle's visual knob position.
// Inputs: `id` identifies the toggle and `enabled` is the final logical state.
// Outputs: Returns a normalized knob position from 0.0 to 1.0.
double MainWindow::toggle_visual_position(ToggleId id, bool enabled) const {
    const double final_position = enabled ? 1.0 : 0.0;
    if (id == ToggleId::None || id != transition_toggle_ ||
        toggle_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return final_position;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               toggle_transition_start_)
                             .count();
    const double progress =
        std::clamp(static_cast<double>(elapsed) / static_cast<double>(kToggleTransitionMs), 0.0, 1.0);
    if (progress >= 1.0) {
        return final_position;
    }
    const double start = transition_toggle_from_ ? 1.0 : 0.0;
    const double end = transition_toggle_to_ ? 1.0 : 0.0;
    return start + ((end - start) * ease_out(progress));
}

// Purpose: Return normalized command-button release animation progress.
// Inputs: `rect` is the button rectangle being rendered.
// Outputs: Returns 1.0 when no release pulse applies to this button.
double MainWindow::button_release_progress(const RECT& rect) const {
    if (button_release_start_ == std::chrono::steady_clock::time_point{} ||
        !contains_point(rect, button_release_point_.x, button_release_point_.y)) {
        return 1.0;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - button_release_start_)
            .count();
    return std::clamp(static_cast<double>(elapsed) / static_cast<double>(kButtonReleaseTransitionMs), 0.0, 1.0);
}

// Purpose: Advance Licenses and History-details smooth-scroll animations.
// Inputs: None; reads active smooth-scroll start and target fields.
// Outputs: Updates visible pixel offsets and returns true while any smooth scroll remains active.
bool MainWindow::tick_smooth_scroll_animation() {
    const auto now = std::chrono::steady_clock::now();
    bool needs_repaint = false;
    const auto advance = [&](int& visible_pixels, int& target_pixels, int& start_pixels,
                             std::chrono::steady_clock::time_point& start_time) {
        if (start_time == std::chrono::steady_clock::time_point{}) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        const double progress =
            std::clamp(static_cast<double>(elapsed) / static_cast<double>(kSmoothScrollTransitionMs), 0.0, 1.0);
        const int next = progress >= 1.0
                             ? target_pixels
                             : static_cast<int>(std::lround(
                                   static_cast<double>(start_pixels) +
                                   ((static_cast<double>(target_pixels - start_pixels)) * ease_out(progress))));
        if (next != visible_pixels) {
            visible_pixels = next;
            needs_repaint = true;
        }
        if (progress >= 1.0) {
            visible_pixels = target_pixels;
            start_pixels = target_pixels;
            start_time = {};
            return needs_repaint;
        }
        return true;
    };
    const bool license_active =
        advance(license_notices_scroll_pixels_, license_notices_scroll_target_pixels_,
                license_notices_scroll_animation_start_pixels_, license_notices_scroll_animation_start_);
    const bool details_active =
        advance(history_details_scroll_pixels_, history_details_scroll_target_pixels_,
                history_details_scroll_animation_start_pixels_, history_details_scroll_animation_start_);
    return license_active || details_active || needs_repaint;
}

// Purpose: Advance active UI animations.
// Inputs: None.
// Outputs: Queues another repaint while animation is active or stops the timer.
void MainWindow::tick_animation() {
    const bool page_active = page_transition_progress() < 1.0;
    const bool toggle_active = transition_toggle_ != ToggleId::None &&
                               toggle_transition_start_ != std::chrono::steady_clock::time_point{} &&
                               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                     toggle_transition_start_)
                                       .count() < kToggleTransitionMs;
    const bool button_release_active =
        button_release_start_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - button_release_start_)
                .count() < kButtonReleaseTransitionMs;
    const bool smooth_scroll_active = tick_smooth_scroll_animation();
    if (!page_active) {
        page_transition_start_ = {};
    }
    if (!toggle_active) {
        transition_toggle_ = ToggleId::None;
        toggle_transition_start_ = {};
    }
    if (!button_release_active) {
        button_release_start_ = {};
        button_release_point_ = POINT{-1, -1};
    }
    if (page_active || toggle_active || button_release_active || smooth_scroll_active) {
        request_repaint();
        return;
    }
    KillTimer(hwnd_, kAnimationTimer);
    request_repaint();
}

// Purpose: Recreate native fonts at the current monitor DPI.
// Inputs: None; reads `dpi_`.
// Outputs: Replaces owned GDI font handles.
void MainWindow::rebuild_fonts() {
    for (HFONT* font : {&title_font_, &body_font_, &small_font_, &tiny_font_, &mono_font_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    title_font_ =
        CreateFontW(-MulDiv(22, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    body_font_ =
        CreateFontW(-MulDiv(12, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    small_font_ =
        CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    tiny_font_ =
        CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    mono_font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                             DEFAULT_PITCH, L"Cascadia Mono");
}

// Purpose: Queue a coalesced repaint on the UI thread.
// Inputs: None.
// Outputs: Posts one repaint request when an HWND exists and no repaint is already queued.
void MainWindow::request_repaint() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (!repaint_queued_.exchange(true)) {
        PostMessageW(hwnd_, WM_APP + 1, 0, 0);
    }
}

// Purpose: Convert design pixels to current monitor pixels.
// Inputs: `value` is a 96-DPI design-pixel value.
// Outputs: Returns the DPI-scaled integer pixel value.
int MainWindow::scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

}  // namespace superzip::app
