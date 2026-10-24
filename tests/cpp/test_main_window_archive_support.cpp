#include "test_environment.hpp"
#include "test_util.hpp"

#include "app/main_window_support.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
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

using flitzzip_test::ScopedEnvironment;

// Purpose: Read an owned test artifact byte-for-byte without text-mode newline conversion.
// Inputs: Existing regular file path.
// Outputs: Returns complete bytes as a string or fails the test when the file cannot be opened.
std::string read_binary_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(static_cast<bool>(input));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Purpose: Locate exact restored content without coupling the GUI route test to adapter-specific output names.
// Inputs: Extraction root and expected file bytes.
// Outputs: Returns true only when a regular descendant has exactly the expected content.
bool contains_file_with_contents(const std::filesystem::path& root, std::string_view expected) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entries(root, error), end; !error && entries != end;
         entries.increment(error)) {
        if (entries->is_regular_file(error) && !error && read_binary_text(entries->path()) == expected) {
            return true;
        }
    }
    REQUIRE_TRUE(!error);
    return false;
}

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

// Purpose: Keep writer-created staging files out of source enumeration without hiding legitimate user files.
// Inputs: Seven folder-capable routes save inside their source, alongside an ordinary temporary-looking subtree.
// Outputs: Each archive contains exactly the original four entries and restores both files byte-for-byte.
TEST_CASE(gui_folder_archives_snapshot_sources_before_creating_staging_files) {
    constexpr std::array formats{flitzzip::ArchiveFormat::Zip,     flitzzip::ArchiveFormat::Tar,
                                 flitzzip::ArchiveFormat::TarGzip, flitzzip::ArchiveFormat::TarBzip2,
                                 flitzzip::ArchiveFormat::TarZstd, flitzzip::ArchiveFormat::Cpio,
                                 flitzzip::ArchiveFormat::CpioGzip};
    const auto root = test_temp_dir("folder-source-snapshot");
    const std::string payload = "ordinary source bytes\n";
    const std::string temporary_named_payload = "intentional user data\n";
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto directory = root / std::to_string(index);
        const auto source = directory / "input";
        std::filesystem::create_directories(source / ".fztmp-kept");
        std::ofstream(source / "source.bin", std::ios::binary) << payload;
        std::ofstream(source / ".fztmp-kept" / "payload.tmp", std::ios::binary) << temporary_named_payload;
        const auto archive = source / "bundle.archive";
        const auto stats = flitzzip::app::compress_gui_archive({source}, archive, formats[index], false, false,
                                                               flitzzip::kDefaultArchiveBlockBytes, 3,
                                                               flitzzip::NativeCompressionMode::Standard, {});
        REQUIRE_EQ(stats.input_bytes, payload.size() + temporary_named_payload.size());
        REQUIRE_EQ(stats.entries, 4U);
        const auto destination = directory / "restored";
        const auto restored =
            flitzzip::app::extract_detected_archive(formats[index], archive, destination, false, false, {});
        REQUIRE_EQ(restored.entries, 4U);
        REQUIRE_EQ(restored.output_bytes, stats.input_bytes);
        for (const auto& [relative, expected] : std::array<std::pair<std::filesystem::path, std::string>, 2>{
                 {{std::filesystem::path("input") / "source.bin", payload},
                  {std::filesystem::path("input") / ".fztmp-kept" / "payload.tmp", temporary_named_payload}}}) {
            std::ifstream input(destination / relative, std::ios::binary);
            REQUIRE_TRUE(input.is_open());
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), expected);
        }
        REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(destination),
                                 std::filesystem::recursive_directory_iterator{}),
                   4);
        REQUIRE_EQ(std::distance(std::filesystem::recursive_directory_iterator(source),
                                 std::filesystem::recursive_directory_iterator{}),
                   4);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prove every GUI-visible compatibility create route reaches a real codec and its matching extractor.
// Inputs: One bounded binary source and every format accepted by `compress_gui_archive`.
// Outputs: Each archive is nonempty, extraction restores exact bytes, and ZIP/general validation discards all output.
TEST_CASE(gui_compatibility_routing_round_trips_all_create_formats) {
    struct Route {
        flitzzip::ArchiveFormat format;
        std::wstring_view extension;
    };
    constexpr std::array<Route, 13> routes{{
        {flitzzip::ArchiveFormat::FlitzZip, L".flzip"},
        {flitzzip::ArchiveFormat::Zip, L".zip"},
        {flitzzip::ArchiveFormat::Tar, L".tar"},
        {flitzzip::ArchiveFormat::TarGzip, L".tar.gz"},
        {flitzzip::ArchiveFormat::TarBzip2, L".tar.bz2"},
        {flitzzip::ArchiveFormat::TarZstd, L".tar.zst"},
        {flitzzip::ArchiveFormat::Gzip, L".gz"},
        {flitzzip::ArchiveFormat::Bzip2, L".bz2"},
        {flitzzip::ArchiveFormat::Zstd, L".zst"},
        {flitzzip::ArchiveFormat::UnixCompress, L".Z"},
        {flitzzip::ArchiveFormat::Cpio, L".cpio"},
        {flitzzip::ArchiveFormat::CpioGzip, L".cpio.gz"},
        {flitzzip::ArchiveFormat::Ar, L".ar"},
    }};
    const auto root = test_temp_dir("gui-compatibility-routes");
    const auto source = root / "source.bin";
    std::string payload;
    payload.reserve(8192U);
    for (std::size_t index = 0; index < 8192U; ++index) {
        payload.push_back(static_cast<char>((index * 37U + index / 17U) & 0xffU));
    }
    {
        std::ofstream output(source, std::ios::binary);
        REQUIRE_TRUE(static_cast<bool>(output));
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        REQUIRE_TRUE(static_cast<bool>(output));
    }

    std::filesystem::path zip_archive;
    std::filesystem::path tar_archive;
    std::filesystem::path flzip_archive;
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto& route = routes[index];
        const auto archive = root / (L"archive-" + std::to_wstring(index) + std::wstring(route.extension));
        const auto stats = flitzzip::app::compress_gui_archive({source}, archive, route.format, false, false,
                                                               flitzzip::kDefaultArchiveBlockBytes, 3,
                                                               flitzzip::NativeCompressionMode::Standard, {});
        REQUIRE_TRUE(std::filesystem::is_regular_file(archive));
        REQUIRE_TRUE(std::filesystem::file_size(archive) > 0U);
        REQUIRE_EQ(stats.input_bytes, payload.size());

        const auto extraction = root / (L"extract-" + std::to_wstring(index));
        const auto restored =
            flitzzip::app::extract_detected_archive(route.format, archive, extraction, false, false, {});
        REQUIRE_EQ(restored.output_bytes, payload.size());
        REQUIRE_TRUE(contains_file_with_contents(extraction, payload));
        if (route.format == flitzzip::ArchiveFormat::FlitzZip) {
            flzip_archive = archive;
        } else if (route.format == flitzzip::ArchiveFormat::Zip) {
            zip_archive = archive;
        } else if (route.format == flitzzip::ArchiveFormat::Tar) {
            tar_archive = archive;
        }
    }

    ScopedEnvironment redirect(L"FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT");
    redirect.set(L"1");
    REQUIRE_EQ(flitzzip::app::validate_detected_archive(flitzzip::ArchiveFormat::FlitzZip, flzip_archive, false, {})
                   .output_bytes,
               payload.size());
    REQUIRE_EQ(
        flitzzip::app::validate_detected_archive(flitzzip::ArchiveFormat::Zip, zip_archive, false, {}).output_bytes,
        payload.size());
    REQUIRE_EQ(
        flitzzip::app::validate_detected_archive(flitzzip::ArchiveFormat::Tar, tar_archive, false, {}).output_bytes,
        payload.size());
    std::filesystem::remove_all(root);
}

// Purpose: Keep unknown, recognized-unsupported, and name-aware GUI extraction routes fail-closed before publication.
// Inputs: An owned Unicode-named empty file and exact format identities that exercise dispatcher-only policy branches.
// Outputs: Stable UTF-8 diagnostics are returned and no extraction destination is published.
TEST_CASE(gui_extraction_dispatch_rejects_unimplemented_routes) {
    const auto root = test_temp_dir("gui-extraction-routing-errors");
    const auto archive = root / std::filesystem::path(u"input-\U0001F9EA.bin");
    {
        std::ofstream output(archive, std::ios::binary);
        REQUIRE_TRUE(static_cast<bool>(output));
    }
    const auto destination = root / "output";
    require_archive_error(
        [&] {
            (void)flitzzip::app::extract_detected_archive(flitzzip::ArchiveFormat::Unknown, archive, destination, false,
                                                          false, {});
        },
        "unable to detect archive format: " + flitzzip::path_diagnostic_utf8(archive));
    require_archive_error(
        [&] {
            (void)flitzzip::app::extract_detected_archive(flitzzip::ArchiveFormat::Rar, archive, destination, false,
                                                          false, {});
        },
        "archive format recognized but not implemented for extraction: rar");
    require_archive_error(
        [&] {
            (void)flitzzip::app::extract_detected_archive(flitzzip::ArchiveFormat::Unknown, archive, destination, false,
                                                          false, {}, static_cast<flitzzip::ArchivePathEncoding>(99));
        },
        "unsupported archive member name encoding");
    bool rpm_rejected = false;
    try {
        (void)flitzzip::app::extract_detected_archive(flitzzip::ArchiveFormat::Rpm, archive, destination, false, false,
                                                      {}, flitzzip::ArchivePathEncoding::HostCodePage);
    } catch (const flitzzip::ArchiveError&) {
        rpm_rejected = true;
    }
    REQUIRE_TRUE(rpm_rejected);
    REQUIRE_TRUE(!std::filesystem::exists(destination));
    std::filesystem::remove_all(root);
}

// Purpose: Prevent persistent-log row injection and verify settings/log storage errors remain transactional.
// Inputs: Control-bearing log text, extreme time, round-tripped settings, missing files, and directory/file confusion.
// Outputs: One encoded row per event, deterministic fallback time, exact settings, and explicit open failures.
TEST_CASE(gui_storage_escapes_log_controls_and_round_trips_settings) {
    const auto root = test_temp_dir("gui-storage-contracts");
    const auto settings_path = root / "settings.json";
    const flitzzip::app::AppSettings defaults;
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(settings_path), defaults));

    auto settings = defaults;
    settings.compression_format_index = 4;
    settings.compression_level_index = 7;
    settings.overwrite = true;
    flitzzip::app::write_settings_file(settings_path, settings);
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(settings_path), settings));

    const auto log_path = root / "flitzzip.log";
    constexpr char injected_bytes[] = "plain\\path\"quoted\"\r\n[ERROR] forged\t\b\f\0\x01\x7f";
    const std::string injected(injected_bytes, sizeof(injected_bytes) - 1U);
    flitzzip::app::append_log_file_entry(log_path, flitzzip::app::LogSeverity::Warning, injected,
                                         std::chrono::system_clock::time_point{});
    const auto first = read_binary_text(log_path);
    REQUIRE_EQ(static_cast<std::size_t>(std::count(first.begin(), first.end(), '\n')), 1U);
    REQUIRE_TRUE(first.find("plain\\\\path\\\"quoted\\\"\\r\\n[ERROR] forged\\t\\b\\f\\u0000\\u0001\\u007f") !=
                 std::string::npos);

    const auto extreme_log = root / "extreme.log";
    flitzzip::app::append_log_file_entry(extreme_log, flitzzip::app::LogSeverity::Information, "event",
                                         std::chrono::system_clock::time_point::min());
    REQUIRE_TRUE(read_binary_text(extreme_log).starts_with("0000-00-00 00:00:00 [Information] event\n"));

    const auto directory = root / "directory";
    std::filesystem::create_directories(directory);
    require_archive_error([&] { (void)flitzzip::app::read_settings_file(directory); },
                          "could not open settings file for reading");
    require_archive_error(
        [&] { flitzzip::app::append_log_file_entry(directory, flitzzip::app::LogSeverity::Information, "event", {}); },
        "could not open FlitzZip log file");

    const auto ordinary_file = root / "ordinary-file";
    {
        std::ofstream output(ordinary_file, std::ios::binary);
        REQUIRE_TRUE(static_cast<bool>(output));
        output << "not a directory";
    }
    require_archive_error([&] { (void)flitzzip::app::read_settings_file(ordinary_file / "child.json"); },
                          "could not inspect settings file");

    ScopedEnvironment redirect(L"FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT");
    ScopedEnvironment temporary(L"TEMP");
    ScopedEnvironment temporary_alias(L"TMP");
    temporary.set(root.c_str());
    temporary_alias.set(root.c_str());
    redirect.set(L"1");
    const auto redirected_storage = flitzzip::app::app_storage_directory();
    REQUIRE_EQ(redirected_storage, root / L"FlitzZip");
    REQUIRE_EQ(flitzzip::app::settings_file_path(), redirected_storage / L"gui-smoke-settings.json");
    flitzzip::app::ensure_app_storage();
    REQUIRE_TRUE(std::filesystem::is_regular_file(redirected_storage / L"flitzzip.log"));

    redirect.set(nullptr);
    const auto storage = flitzzip::app::app_storage_directory();
    REQUIRE_TRUE(!storage.empty());
    REQUIRE_EQ(flitzzip::app::settings_file_path(), storage / L"settings.json");
    REQUIRE_EQ(flitzzip::app::log_file_path(), storage / L"flitzzip.log");
    std::filesystem::remove_all(root);
}

// Purpose: Exercise every dropdown selection branch and normalization boundary independently of rendering.
// Inputs: Distinct state rows, stale name-encoding selections, and invalid enum values.
// Outputs: Each selection maps to its owning state field and invalid identifiers remain inert.
TEST_CASE(gui_dropdown_selection_and_name_encoding_are_bounded) {
    flitzzip::app::UiState state;
    state.compression_format_index = 4;
    state.compression_level_index = 5;
    state.compression_block_size_index = 6;
    state.extract_name_encoding_index = 1;
    state.history_operation_filter_index = 3;
    state.history_status_filter_index = 2;
    state.performance_update_seconds = 5;
    state.gpu_required = false;
    // The first row is invariant across host inventories; explicit inventory tests cover stale-index arithmetic.
    state.io_drive_index = 0;
    state.log_level_index = 2;
    state.log_retention_index = 1;
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::CompressFormat), 4);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::CompressLevel), 5);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::CompressMethod), 1);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::CompressBlockSize), 6);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::ExtractOverwrite), 0);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::ExtractNameEncoding), 1);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::HistoryOperation), 3);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::HistoryStatus), 2);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::GpuUpdateSpeed), 2);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::SystemIoDrive), 0);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::SettingsLogLevel), 2);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::SettingsLogRetention), 1);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, flitzzip::app::DropdownId::None), 0);
    REQUIRE_EQ(flitzzip::app::dropdown_selected_index(state, static_cast<flitzzip::app::DropdownId>(999)), 0);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::CompressMethod, state).size(), 2U);
    REQUIRE_EQ(flitzzip::app::dropdown_options(flitzzip::app::DropdownId::ExtractOverwrite, state).size(), 2U);
    REQUIRE_TRUE(flitzzip::app::dropdown_options(static_cast<flitzzip::app::DropdownId>(999), state).empty());

    flitzzip::app::UiState default_destination;
    REQUIRE_EQ(flitzzip::app::destination_directory_or_default(default_destination),
               flitzzip::app::current_user_downloads_directory());
    REQUIRE_EQ(flitzzip::app::extraction_output_path_for(default_destination),
               flitzzip::app::current_user_downloads_directory() / L"FlitzZip-extracted");
    REQUIRE_TRUE(flitzzip::app::compression_format_uses_flzip_tuning(flitzzip::ArchiveFormat::FlitzZip));
    REQUIRE_TRUE(!flitzzip::app::compression_format_uses_flzip_tuning(flitzzip::ArchiveFormat::Zip));
    REQUIRE_TRUE(flitzzip::app::compression_format_uses_level(flitzzip::ArchiveFormat::Zstd));
    REQUIRE_TRUE(!flitzzip::app::compression_format_uses_level(flitzzip::ArchiveFormat::Tar));
    REQUIRE_EQ(flitzzip::app::compression_block_size_bytes(-1), flitzzip::app::kCompressionBlockSizeOptions.back());
    REQUIRE_EQ(flitzzip::app::compression_block_size_bytes(flitzzip::app::kCompressionBlockSizeMaxIndex + 1),
               flitzzip::app::kCompressionBlockSizeOptions.front());
    const auto drives = flitzzip::app::fixed_io_drive_options();
    const auto drive_index = flitzzip::app::normalize_io_drive_index(999);
    REQUIRE_EQ(flitzzip::app::io_drive_option_text(999), drives.empty() ? L"-" : drives[drive_index]);

    const std::array<std::filesystem::path, 2> encoded_candidates{L"archive.ar", L"archive.zip"};
    REQUIRE_TRUE(flitzzip::app::extract_name_encoding_available(encoded_candidates));
    const std::array<std::filesystem::path, 1> utf8_only{L"archive.zip"};
    REQUIRE_TRUE(!flitzzip::app::extract_name_encoding_available(utf8_only));

    state.extract_name_encoding_index = -50;
    REQUIRE_EQ(flitzzip::app::selected_name_encoding(state).encoding, flitzzip::ArchivePathEncoding::Utf8);
    state.extract_name_encoding_index = 50;
    REQUIRE_EQ(flitzzip::app::selected_name_encoding(state).encoding, flitzzip::ArchivePathEncoding::HostCodePage);
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
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 999999U);
    auto_close.set(L"910000");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 910000U);
    auto_close.set(L"4294967295");
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), static_cast<UINT>(USER_TIMER_MAXIMUM));
    for (const auto* invalid : {L"-1", L" 15000", L"15000suffix", L"4294967296"}) {
        auto_close.set(invalid);
        REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 0U);
    }
    const std::wstring oversized_number(40, L'9');
    auto_close.set(oversized_number.c_str());
    REQUIRE_EQ(flitzzip::app::smoke_auto_close_ms(), 0U);
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

// Purpose: Keep opaque exception bytes from turning status rendering into a second conversion failure.
// Inputs: Empty, valid Unicode and malformed UTF-8 diagnostics with literal escape-like text.
// Outputs: Exact valid text or explicitly labeled reversible byte escapes; strict metadata conversion still rejects.
TEST_CASE(diagnostic_text_preserves_unicode_and_labels_invalid_exception_bytes) {
    using flitzzip::app::diagnostic_text;
    REQUIRE_EQ(diagnostic_text(""), std::wstring());
    REQUIRE_EQ(diagnostic_text("Error: \xCE\xB4\xF0\x9F\x93\xA6"), std::wstring(L"Error: \u03B4\U0001F4E6"));
    REQUIRE_EQ(diagnostic_text(std::string("Error: \xFF\\x00\n", 14)),
               std::wstring(L"[invalid UTF-8 bytes] Error: \\xFF\\x5Cx00\\x0A\\x00"));
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::app::widen("\xFF"));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
