#include "test_util.hpp"

#include "app/main_window_support.hpp"
#include "core/result.hpp"

#include <fstream>
#include <limits>
#include <utility>

namespace {

// Purpose: Require one production boundary to reject input with an exact stable diagnostic.
// Inputs: Callable `operation` and expected ArchiveError text.
// Outputs: Returns normally only when the exact failure contract is observed.
template <typename Operation> void require_archive_error(Operation&& operation, std::string_view expected) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::ArchiveError& error) {
        rejected = true;
        REQUIRE_EQ(std::string(error.what()), std::string(expected));
    }
    REQUIRE_TRUE(rejected);
}

class ScopedEnvironment final {
  public:
    // Purpose: Preserve one process environment value while a GUI-startup contract mutates it.
    // Inputs: `name` is a stable variable owned by the test process.
    // Outputs: Captures the prior value and restores it at destruction.
    explicit ScopedEnvironment(std::wstring name) : name_(std::move(name)) {
        wchar_t buffer[32768]{};
        const DWORD length = GetEnvironmentVariableW(name_.c_str(), buffer, 32768U);
        if (length > 0U && length < 32768U) {
            previous_.assign(buffer, length);
            existed_ = true;
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    // Purpose: Restore the captured environment value without leaking test state.
    // Inputs: Captured construction state.
    // Outputs: Restores or removes the variable.
    ~ScopedEnvironment() {
        SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr);
    }

    // Purpose: Set or remove the scoped environment variable.
    // Inputs: Nullable UTF-16 value.
    // Outputs: Fails the test if Windows rejects the mutation.
    void set(const wchar_t* value) const {
        REQUIRE_TRUE(SetEnvironmentVariableW(name_.c_str(), value) != 0);
    }

  private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

}  // namespace

// Purpose: Keep shared GUI text conversion strict and byte-unit output stable.
// Inputs: Empty, ASCII, multibyte UTF-8, exact binary thresholds, and invalid telemetry.
// Outputs: Valid text survives exactly; malformed UTF-8 and invalid byte counts fail visibly.
TEST_CASE(gui_text_formatting_is_strict_and_deterministic) {
    REQUIRE_TRUE(flitzzip::app::widen("").empty());
    REQUIRE_EQ(flitzzip::app::widen("FlitzZip"), L"FlitzZip");
    REQUIRE_EQ(flitzzip::app::widen("\xE2\x98\x85"), L"\u2605");
    require_archive_error([] { (void)flitzzip::app::widen("\xC3\x28"); }, "text is not valid UTF-8");

    REQUIRE_EQ(flitzzip::app::human_bytes(0.0), "0 B");
    REQUIRE_EQ(flitzzip::app::human_bytes(1023.0), "1023 B");
    REQUIRE_EQ(flitzzip::app::human_bytes(1536.0), "1.5 KiB");
    REQUIRE_EQ(flitzzip::app::human_bytes(1024.0 * 1024.0), "1.0 MiB");
    require_archive_error([] { (void)flitzzip::app::human_bytes(-1.0); }, "byte count must be finite and nonnegative");
    require_archive_error([] { (void)flitzzip::app::human_bytes(std::numeric_limits<double>::infinity()); },
                          "byte count must be finite and nonnegative");
}

// Purpose: Bind every GUI create row to the core archive registry rather than duplicated labels.
// Inputs: All selector rows plus negative and overflowing circular selections.
// Outputs: Format, extension, and label remain registry-owned and normalization stays deterministic.
TEST_CASE(gui_archive_selector_matches_core_registry) {
    for (int index = 0; index <= flitzzip::app::kCompressionFormatMaxIndex; ++index) {
        const auto extension = flitzzip::app::kCompressionCreateFormatExtensions[static_cast<std::size_t>(index)];
        const auto& registered = flitzzip::archive_format_extension_info_for_extension(extension);
        REQUIRE_TRUE(registered.can_create);
        REQUIRE_EQ(flitzzip::app::compression_format_value(index), registered.format);
        REQUIRE_EQ(flitzzip::app::compression_format_extension(index), flitzzip::app::widen(registered.extension));
        REQUIRE_EQ(flitzzip::app::compression_format_text(index), flitzzip::app::widen(registered.display_name));
    }
    REQUIRE_EQ(flitzzip::app::compression_format_value(-1),
               flitzzip::app::compression_format_value(flitzzip::app::kCompressionFormatMaxIndex));
    REQUIRE_EQ(flitzzip::app::compression_format_value(flitzzip::app::kCompressionFormatMaxIndex + 1),
               flitzzip::app::compression_format_value(0));

    flitzzip::app::UiState state;
    state.destination_directory = L"C:\\FlitzZip-test-output";
    state.compression_format_index = 1;
    REQUIRE_EQ(flitzzip::app::compression_output_filename_for(state), L"FlitzZip-output.zip");
    REQUIRE_EQ(flitzzip::app::compression_output_path_for(state), state.destination_directory / L"FlitzZip-output.zip");
    REQUIRE_EQ(flitzzip::app::extraction_output_path_for(state), state.destination_directory);
}

// Purpose: Enforce the separate GPU-only Neutron choice across capability, policy, and format changes.
// Inputs: FLZIP/ZIP states with all combinations of cached HIP availability and required-GPU preference.
// Outputs: Neutron appears only for eligible FLZIP and is downgraded explicitly instead of falling back.
TEST_CASE(gui_neutron_selection_fails_closed) {
    flitzzip::app::UiState state;
    state.compression_format_index = 0;
    state.compression_level_index = flitzzip::app::kNeutronCompressionLevelIndex;
    state.gpu_available = true;
    state.gpu_required = true;
    REQUIRE_TRUE(flitzzip::app::neutron_selection_available(state));
    REQUIRE_TRUE(!flitzzip::app::normalize_compression_selection(state));

    state.gpu_required = false;
    REQUIRE_TRUE(!flitzzip::app::neutron_selection_available(state));
    REQUIRE_TRUE(flitzzip::app::normalize_compression_selection(state));
    REQUIRE_EQ(state.compression_level_index, flitzzip::app::kCompressionLevelOptionCount - 1);
    REQUIRE_TRUE(state.status.find("requires FLZIP") != std::string::npos);

    state.compression_level_index = flitzzip::app::kNeutronCompressionLevelIndex;
    state.gpu_required = true;
    state.compression_format_index = 1;
    REQUIRE_TRUE(flitzzip::app::normalize_compression_selection(state));
    REQUIRE_EQ(state.compression_level_index, flitzzip::app::kCompressionLevelOptionCount - 1);
}

// Purpose: Verify settings migration, capability normalization, and deterministic persistence through production code.
// Inputs: A legacy v1 row and a current all-options snapshot including separate Neutron intent.
// Outputs: Legacy format/effort semantics survive and current settings round-trip exactly when eligible.
TEST_CASE(gui_settings_migrate_and_round_trip) {
    const auto legacy = flitzzip::app::parse_settings_json(
        R"({"schema":"flitzzip.settings.v1","compressionFormatIndex":3,"compressionLevelIndex":4})");
    REQUIRE_EQ(flitzzip::app::compression_format_extension(legacy.compression_format_index), L".tar.gz");
    REQUIRE_EQ(legacy.compression_level_index, 8);

    flitzzip::app::AppSettings settings;
    settings.compression_format_index = 0;
    settings.compression_level_index = flitzzip::app::kNeutronCompressionLevelIndex;
    settings.compression_block_size_index = 6;
    settings.log_level_index = 2;
    settings.log_retention_index = 2;
    settings.performance_update_seconds = 10;
    settings.open_destination_after_operation = true;
    settings.show_operation_summary = false;
    settings.validate_before_publish = false;
    settings.open_destination_after_extract = true;
    settings.gpu_required = true;
    settings.overwrite = true;
    settings.integrity_hash_opt_in = true;
    settings.defender_scan_opt_in = true;
    settings.verify_after_write_opt_in = true;
    const auto serialized = flitzzip::app::settings_to_json(settings);
    const auto reparsed = flitzzip::app::parse_settings_json(serialized);
    REQUIRE_TRUE(flitzzip::app::settings_equal(settings, reparsed));

    flitzzip::app::UiState unavailable;
    unavailable.gpu_available = false;
    flitzzip::app::apply_settings_to_state(reparsed, unavailable);
    REQUIRE_EQ(unavailable.compression_level_index, flitzzip::app::kCompressionLevelOptionCount - 1);

    flitzzip::app::UiState available;
    available.gpu_available = true;
    flitzzip::app::apply_settings_to_state(reparsed, available);
    REQUIRE_EQ(available.compression_level_index, flitzzip::app::kNeutronCompressionLevelIndex);
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::settings_from_state(available), settings));
}

// Purpose: Verify dropdown contents and selections share the same production capability policies.
// Inputs: Default state, then an eligible Neutron state and representative selected values.
// Outputs: Row counts, labels, and selected indices remain synchronized.
TEST_CASE(gui_dropdown_contracts_share_production_state) {
    flitzzip::app::UiState state;
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::CompressFormat, state).size(),
               flitzzip::app::kCompressionCreateFormatExtensions.size());
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::CompressLevel, state).size(), 9U);
    state.gpu_available = true;
    state.gpu_required = true;
    const auto levels = flitzzip::app::dropdown_options(flitzzip::app::DropdownId::CompressLevel, state);
    REQUIRE_EQ(levels.size(), 10U);
    REQUIRE_EQ(levels.back(), L"Neutron star mode");
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::CompressBlockSize, state).size(), 7U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::ExtractNameEncoding, state).size(), 2U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::HistoryOperation, state).size(), 5U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::HistoryStatus, state).size(), 3U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::GpuUpdateSpeed, state).size(), 4U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::SettingsLogLevel, state).size(), 3U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::SettingsLogRetention, state).size(), 3U);
    REQUIRE_TRUE(!flitzzip::app::dropdown_options(flitzzip::app::DropdownId::SystemIoDrive, state).empty());
    REQUIRE_TRUE(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::None, state).empty());

    state.overwrite = true;
    state.performance_update_seconds = 9;
    state.log_level_index = 2;
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::ExtractOverwrite), 1);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::GpuUpdateSpeed), 3);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::SettingsLogLevel), 2);
}

// Purpose: Keep GUI history summaries stable across all security-result states.
// Inputs: File/directory integrity results, four Defender outcomes, and operation labels.
// Outputs: User-visible text remains complete and operation filters remain deterministic.
TEST_CASE(gui_history_status_contracts_are_complete) {
    flitzzip::IntegrityResult file;
    file.algorithm = "SHA-256";
    file.hex_digest = "abc";
    file.target = "file";
    REQUIRE_EQ(flitzzip::app::integrity_history_status("Input", file), "Input SHA-256 abc");
    file.target = "directory";
    file.files_hashed = 2;
    file.directories_hashed = 1;
    file.bytes_hashed = 1536;
    REQUIRE_EQ(flitzzip::app::integrity_history_status("Input", file),
               "Input SHA-256 abc (2 files, 1 folders, 1.5 KiB)");

    flitzzip::DefenderScanResult scan;
    REQUIRE_EQ(flitzzip::app::defender_history_status("Input", scan), "Input scan unavailable");
    scan.attempted = true;
    REQUIRE_EQ(flitzzip::app::defender_history_status("Input", scan), "Input scan not clean");
    scan.timed_out = true;
    REQUIRE_EQ(flitzzip::app::defender_history_status("Input", scan), "Input scan timed out");
    scan.clean = true;
    REQUIRE_EQ(flitzzip::app::defender_history_status("Input", scan), "Input scan clean");

    REQUIRE_EQ(flitzzip::app::operation_for_job_label("Compressing"), "Compress");
    REQUIRE_EQ(flitzzip::app::operation_for_job_label("Extracting"), "Extract");
    REQUIRE_EQ(flitzzip::app::operation_for_job_label("Verifying"), "Security");
    REQUIRE_EQ(flitzzip::app::operation_for_job_label("Idle"), "Failure");
}

// Purpose: Reject invalid GUI backend combinations before any filesystem operation begins.
// Inputs: Empty sources with incompatible mode, verification, unknown, and unsupported-format selections.
// Outputs: Each request fails at the routing boundary with its precise policy diagnostic.
TEST_CASE(gui_compression_routing_rejects_invalid_combinations_before_io) {
    const std::vector<std::filesystem::path> sources;
    const std::filesystem::path output = L"C:\\must-not-be-created.flzip";
    const flitzzip::ProgressCallback progress;
    require_archive_error(
        [&] {
            (void)flitzzip::app::compress_gui_archive(sources, output, flitzzip::ArchiveFormat::Zip, false, false,
                                                      flitzzip::kDefaultArchiveBlockBytes, 9,
                                                      flitzzip::NativeCompressionMode::NeutronStar, progress);
        },
        "Neutron star mode is available only for FLZIP");
    require_archive_error(
        [&] {
            (void)flitzzip::app::compress_gui_archive(sources, output, flitzzip::ArchiveFormat::Tar, false, true,
                                                      flitzzip::kDefaultArchiveBlockBytes, 5,
                                                      flitzzip::NativeCompressionMode::Standard, progress);
        },
        "verify-after-write is currently supported only for FLZIP and ZIP");
    require_archive_error(
        [&] {
            (void)flitzzip::app::compress_gui_archive(sources, output, flitzzip::ArchiveFormat::Rar, false, false,
                                                      flitzzip::kDefaultArchiveBlockBytes, 5,
                                                      flitzzip::NativeCompressionMode::Standard, progress);
        },
        "archive format recognized but not implemented for compression: rar");
}

// Purpose: Validate fixed-window and smoke-control startup policy without launching the GUI.
// Inputs: Missing, malformed, low, high, and valid environment values plus an isolated close marker.
// Outputs: Window style remains non-resizable and smoke controls parse, clamp, and restore state exactly.
TEST_CASE(gui_startup_policy_is_bounded_and_non_resizable) {
    const DWORD style = flitzzip::app::window_style();
    REQUIRE_TRUE((style & WS_CAPTION) != 0U);
    REQUIRE_TRUE((style & WS_MINIMIZEBOX) != 0U);
    REQUIRE_TRUE((style & WS_THICKFRAME) == 0U);
    REQUIRE_TRUE((style & WS_MAXIMIZEBOX) == 0U);
    REQUIRE_TRUE(!flitzzip::app::current_user_downloads_directory().empty());
    REQUIRE_TRUE(flitzzip::app::paths_from_hdrop(nullptr).empty());

    ScopedEnvironment auto_close(L"FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS");
    auto_close.set(nullptr);
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 0U);
    auto_close.set(L"invalid");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 0U);
    auto_close.set(L"1");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 5000U);
    auto_close.set(L"999999");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 300000U);
    auto_close.set(L"15000");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 15000U);

    const auto root = test_temp_dir("gui_smoke_close");
    const auto marker = root / "close.marker";
    ScopedEnvironment close_file(L"FLITZZIP_GUI_SMOKE_CLOSE_FILE");
    close_file.set(marker.c_str());
    REQUIRE_EQ(flitzzip::app::smoke_close_marker_path(), marker);
    REQUIRE_TRUE(!flitzzip::app::smoke_close_requested());
    {
        std::ofstream output(marker);
        output << "close";
    }
    REQUIRE_TRUE(flitzzip::app::smoke_close_requested());
    std::filesystem::remove_all(root);
}
