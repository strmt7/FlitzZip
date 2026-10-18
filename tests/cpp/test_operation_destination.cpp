#include "app/extraction_destinations.hpp"
#include "app/operation_destination.hpp"
#include "test_util.hpp"

#include <fstream>

TEST_CASE(extraction_confirmation_requires_proven_empty_destination) {
    using flitzzip::app::extraction_destination_needs_confirmation;
    const auto root = test_temp_dir("extraction-confirmation");
    REQUIRE_TRUE(!extraction_destination_needs_confirmation(root / "absent"));
    REQUIRE_TRUE(!extraction_destination_needs_confirmation(root));
    const auto child = root / "child";
    std::filesystem::create_directory(child);
    REQUIRE_TRUE(extraction_destination_needs_confirmation(root));
    REQUIRE_TRUE(!extraction_destination_needs_confirmation(child));
    const auto file = child / "data";
    const HANDLE fixture =
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(fixture != INVALID_HANDLE_VALUE);
    REQUIRE_TRUE(CloseHandle(fixture) != FALSE);
    REQUIRE_TRUE(extraction_destination_needs_confirmation(file));
    REQUIRE_TRUE(extraction_destination_needs_confirmation(child));
    std::filesystem::remove_all(root);
}

TEST_CASE(operation_destination_uses_output_and_captured_preference_for_each_job_kind) {
    using flitzzip::OperationKind;
    using flitzzip::app::operation_destination_path;
    const auto directory = std::filesystem::path(u8"C:/output folder/\u03a9");
    const auto archive = directory / L"archive.flzip";
    for (const bool global : {false, true}) {
        for (const bool extraction : {false, true}) {
            REQUIRE_EQ(operation_destination_path(OperationKind::Compress, archive, global, extraction),
                       global ? directory : std::filesystem::path{});
            REQUIRE_EQ(operation_destination_path(OperationKind::Extract, directory, global, extraction),
                       (global || extraction) ? directory : std::filesystem::path{});
            REQUIRE_TRUE(operation_destination_path(OperationKind::Verify, archive, global, extraction).empty());
            REQUIRE_TRUE(operation_destination_path(OperationKind::Idle, archive, global, extraction).empty());
        }
    }
    REQUIRE_TRUE(operation_destination_path(OperationKind::Compress, {}, true, true).empty());
    REQUIRE_TRUE(operation_destination_path(OperationKind::Extract, {}, true, true).empty());
}

TEST_CASE(operation_destination_is_success_only_and_consumed_once) {
    flitzzip::app::OperationDestination completion;
    const auto folder = std::filesystem::path(u8"C:/output/\u03a9");
    REQUIRE_TRUE(completion.take().empty());
    completion.begin(folder);
    REQUIRE_TRUE(completion.take().empty());
    completion.complete(true);
    REQUIRE_EQ(completion.take(), folder);
    REQUIRE_TRUE(completion.take().empty());
    completion.complete(true);
    REQUIRE_TRUE(completion.take().empty());
    completion.begin(folder);
    completion.complete(false);
    REQUIRE_TRUE(completion.take().empty());
    completion.complete(true);
    REQUIRE_TRUE(completion.take().empty());
    completion.begin({});
    completion.complete(true);
    REQUIRE_TRUE(completion.take().empty());
}

TEST_CASE(operation_destination_new_job_drops_stale_output_and_owns_its_snapshot) {
    flitzzip::app::OperationDestination completion;
    std::filesystem::path folder = L"C:/original";
    completion.begin(folder);
    folder = L"C:/changed";
    completion.complete(true);
    REQUIRE_EQ(completion.take(), std::filesystem::path(L"C:/original"));
    completion.begin(folder);
    completion.complete(true);
    completion.begin(L"C:/next");
    REQUIRE_TRUE(completion.take().empty());
    completion.complete(true);
    REQUIRE_EQ(completion.take(), std::filesystem::path(L"C:/next"));
}

// Purpose: Preserve explicit single-archive destinations and reject inadmissible batch cardinality before I/O.
// Inputs: Empty, single and over-capacity archive lists with a caller-selected Unicode destination.
// Outputs: No outputs for no inputs; exact root for one archive; oversized batches fail explicitly.
TEST_CASE(extraction_outputs_preserve_single_root_and_enforce_queue_capacity) {
    const std::filesystem::path root(L"C:/selected destination/\u03a9");
    REQUIRE_TRUE(flitzzip::app::extraction_outputs_for_archives({}, root).empty());
    const std::vector<std::filesystem::path> single{L"incoming/archive.tar.gz"};
    REQUIRE_EQ(flitzzip::app::extraction_outputs_for_archives(single, root), std::vector<std::filesystem::path>{root});
    std::vector<std::filesystem::path> oversized(flitzzip::app::kMaxQueueItems + 1U, L"archive.zip");
    bool rejected = false;
    try {
        (void)flitzzip::app::extraction_outputs_for_archives(oversized, root);
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string_view(error.what()).find("Queue item limit") != std::string_view::npos;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Keep case-equivalent archives and existing numeric suffixes in separate batch destinations.
// Inputs: Different source directories and formats with overlapping Windows filename identities.
// Outputs: Independently specified distinct names, preserving spelling and complete compound-extension removal.
TEST_CASE(extraction_outputs_disambiguate_case_and_existing_suffixes) {
    const std::filesystem::path root(L"C:/selected");
    const std::vector<std::filesystem::path> archives{L"one/Report.zip", L"two/report.TAR.GZ",
                                                      L"three/REPORT (2).flzip", L"four/Report.7z"};
    const std::vector<std::filesystem::path> expected{root / L"Report", root / L"report (2)", root / L"REPORT (2) (2)",
                                                      root / L"Report (3)"};
    REQUIRE_EQ(flitzzip::app::extraction_outputs_for_archives(archives, root), expected);
}

// Purpose: Check Unicode case collisions against real Windows directory identity and independent payload readback.
// Inputs: Two Latin and two Greek case-equivalent names, each assigned different same-name file contents.
// Outputs: Every planned directory is created separately and each original payload remains unchanged.
TEST_CASE(extraction_outputs_keep_unicode_case_collisions_physically_separate) {
    const auto root = test_temp_dir("batch-extraction-names");
    const std::vector<std::filesystem::path> archives{L"first/\u00c5ngstr\u00f6m.zip", L"second/\u00e5NGSTR\u00d6M.tar",
                                                      L"third/\u03a9.flzip", L"fourth/\u03c9.cpio"};
    const auto outputs = flitzzip::app::extraction_outputs_for_archives(archives, root);
    REQUIRE_EQ(outputs.size(), archives.size());
    const std::vector<std::wstring> names{L"\u00c5ngstr\u00f6m", L"\u00e5NGSTR\u00d6M (2)", L"\u03a9", L"\u03c9 (2)"};
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        REQUIRE_EQ(outputs[index].filename().native(), names[index]);
        REQUIRE_EQ(outputs[index].parent_path(), root);
        REQUIRE_TRUE(std::filesystem::create_directory(outputs[index]));
        std::ofstream file(outputs[index] / "same.txt", std::ios::binary);
        file << "independent payload " << index;
        file.close();
        REQUIRE_TRUE(file.good());
    }
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        std::ifstream file(outputs[index] / "same.txt", std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        REQUIRE_EQ(actual, "independent payload " + std::to_string(index));
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            REQUIRE_TRUE(!std::filesystem::equivalent(outputs[earlier], outputs[index]));
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep duplicate suffixes within the Windows component limit without breaking supplementary characters.
// Inputs: Twelve maximum-length archive filenames whose last stem character occupies a UTF-16 pair.
// Outputs: Real distinct directories, valid UTF-16, and full suffixes including the two-digit transition.
TEST_CASE(extraction_outputs_bound_long_unicode_components_and_suffixes) {
    const auto root = test_temp_dir("batch-long-extraction-names");
    const auto base = std::wstring(249U, L'x') + L"\U0001f4c1";
    const std::vector<std::filesystem::path> archives(12U, base + L".zip");
    const auto outputs = flitzzip::app::extraction_outputs_for_archives(archives, root);
    REQUIRE_EQ(outputs.size(), archives.size());
    REQUIRE_EQ(outputs[0].filename().native(), base);
    REQUIRE_EQ(outputs[8].filename().native(), base + L" (9)");
    REQUIRE_EQ(outputs[9].filename().native(), std::wstring(249U, L'x') + L" (10)");
    for (const auto& output : outputs) {
        const auto component = output.filename().native();
        REQUIRE_TRUE(component.size() <= 255U);
        REQUIRE_TRUE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, component.data(),
                                         static_cast<int>(component.size()), nullptr, 0, nullptr, nullptr) > 0);
        REQUIRE_TRUE(std::filesystem::create_directory(output));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Retain derived-name sanitization while disambiguating names that become equal after sanitization.
// Inputs: Forbidden filename characters, empty visible stems, and an unknown-extension input.
// Outputs: Independently specified relative components; no component escapes the selected parent.
TEST_CASE(extraction_outputs_disambiguate_sanitized_names) {
    const std::filesystem::path root(L"C:/selected");
    const std::vector<std::filesystem::path> archives{L"alpha?.zip", L"alpha*.tar", L"...", L"  .tar", L"plain.custom"};
    const std::vector<std::filesystem::path> expected{root / L"alpha_", root / L"alpha_ (2)", root / L"archive",
                                                      root / L"archive (2)", root / L"plain"};
    REQUIRE_EQ(flitzzip::app::extraction_outputs_for_archives(archives, root), expected);
}

// Purpose: Admit the full Queue capacity without losing input order or assigning shared output paths.
// Inputs: Maximum-sized batches of distinct and identical stems; no filesystem payload is created.
// Outputs: Exactly one stable caller-rooted destination per input, including both boundary items.
TEST_CASE(extraction_outputs_admit_the_complete_queue_capacity) {
    std::vector<std::filesystem::path> archives;
    archives.reserve(flitzzip::app::kMaxQueueItems);
    for (std::size_t index = 0; index < flitzzip::app::kMaxQueueItems; ++index) {
        archives.emplace_back(std::to_wstring(index) + L".zip");
    }
    const std::filesystem::path root(L"C:/selected");
    const auto outputs = flitzzip::app::extraction_outputs_for_archives(archives, root);
    REQUIRE_EQ(outputs.size(), archives.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        REQUIRE_EQ(outputs[index], root / std::to_wstring(index));
    }
    archives.assign(flitzzip::app::kMaxQueueItems, L"same.zip");
    const auto duplicates = flitzzip::app::extraction_outputs_for_archives(archives, root);
    REQUIRE_EQ(duplicates.size(), archives.size());
    REQUIRE_EQ(duplicates.front(), root / L"same");
    for (std::size_t index = 1; index < duplicates.size(); ++index) {
        REQUIRE_EQ(duplicates[index], root / (L"same (" + std::to_wstring(index + 1U) + L")"));
    }
}
