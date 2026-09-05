#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "wim/wim_adapter.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200)
#endif
#include "wimlib.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace {

#ifndef SUPERZIP_TEST_FIXTURES_DIR
#define SUPERZIP_TEST_FIXTURES_DIR "tests/fixtures"
#endif

// Purpose: Resolve a pinned WIM fixture path for tests.
// Inputs: `name` is the fixture filename under `tests/fixtures/wim`.
// Outputs: Returns the absolute or source-relative fixture path.
std::filesystem::path wim_fixture_path(const std::string& name) {
    return std::filesystem::path(SUPERZIP_TEST_FIXTURES_DIR) / "wim" / name;
}

// Purpose: Read a small text fixture result.
// Inputs: `path` is an extracted text file.
// Outputs: Returns the full text payload or throws on I/O failure.
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open text file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Resolve a fixture writer export from the already verified product runtime.
// Inputs: `module` is the loaded wimlib handle and `name` identifies an exact header-declared export.
// Outputs: Returns a typed function pointer or fails the test when the export is absent.
template <typename Function> Function require_wim_export(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    REQUIRE_TRUE(address != nullptr);
    return reinterpret_cast<Function>(address);
}

// Purpose: Create a bounded Unicode WIM fixture using the pinned library's independent writer API.
// Inputs: `source` is a tiny test tree, `archive` is its output path, and `images` is one or two.
// Outputs: Writes an integrity-checked WIM without capturing ACLs; requires prior product runtime initialization.
void write_unicode_wim_fixture(const std::filesystem::path& source, const std::filesystem::path& archive, int images) {
    const auto module = GetModuleHandleW(L"libwim-15.dll");
    REQUIRE_TRUE(module != nullptr);
    const auto create = require_wim_export<decltype(&wimlib_create_new_wim)>(module, "wimlib_create_new_wim");
    const auto add = require_wim_export<decltype(&wimlib_add_image)>(module, "wimlib_add_image");
    const auto write = require_wim_export<decltype(&wimlib_write)>(module, "wimlib_write");
    const auto release = require_wim_export<decltype(&wimlib_free)>(module, "wimlib_free");
    WIMStruct* raw = nullptr;
    const auto status = create(WIMLIB_COMPRESSION_TYPE_NONE, &raw);
    const std::unique_ptr<WIMStruct, decltype(release)> wim(raw, release);
    REQUIRE_EQ(status, 0);
    REQUIRE_TRUE(wim != nullptr);
    for (int index = 1; index <= images; ++index) {
        const auto name = L"image-" + std::to_wstring(index);
        REQUIRE_EQ(add(wim.get(), source.c_str(), name.c_str(), nullptr,
                       WIMLIB_ADD_FLAG_NO_ACLS | WIMLIB_ADD_FLAG_NO_UNSUPPORTED_EXCLUDE),
                   0);
    }
    REQUIRE_EQ(write(wim.get(), archive.c_str(), WIMLIB_ALL_IMAGES, WIMLIB_WRITE_FLAG_CHECK_INTEGRITY, 1U), 0);
}

}  // namespace

TEST_CASE(wim_fixture_extracts_with_native_adapter) {
    const auto root = test_temp_dir("wim-extract");
    const auto destination = root / "out";
    const auto fixture = wim_fixture_path("basic.wim");

    REQUIRE_EQ(superzip::detect_archive_format(fixture), superzip::ArchiveFormat::Wim);
    const auto& info = superzip::archive_format_info(superzip::ArchiveFormat::Wim);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);

    const auto stats = superzip::extract_wim(fixture, destination, false);
    REQUIRE_TRUE(stats.entries >= 2U);
    REQUIRE_EQ(read_text(destination / "root.txt"), std::string("wim-root"));
    REQUIRE_EQ(read_text(destination / "nested" / "hello.txt"), std::string("hello wim"));
    superzip_test::export_compat_fixture(fixture, destination);
}

// Purpose: Preserve UTF-16 WIM names in private staging and single/multiple-image publication.
// Inputs: Tiny library-generated images with accented, CJK, supplementary, empty-file, and directory names.
// Outputs: Checks exact payloads and names, explicit overwrite, and private staging cleanup.
TEST_CASE(wim_unicode_names_extract_and_overwrite) {
    const auto root = test_temp_dir("wim-unicode") / L"\u65e5\u672c";
    std::filesystem::create_directories(root);
    (void)superzip::extract_wim(wim_fixture_path("basic.wim"), root / "initialize", false);
    const auto source = root / L"source-\u03a9";
    const auto relative = std::filesystem::path(L"caf\u00e9") / L"\u65e5\u672c-\U0001f680.txt";
    std::filesystem::create_directories((source / relative).parent_path());
    std::filesystem::create_directories(source / L"empty-\u03a9");
    std::ofstream(source / relative, std::ios::binary) << "wim unicode payload\n";
    std::ofstream(source / L"empty-\u00e9.txt", std::ios::binary);
    for (const int images : {1, 2}) {
        const auto archive = root / (L"archive-\u03a9-" + std::to_wstring(images) + L".wim");
        const auto output = root / (L"output-\u00e9-" + std::to_wstring(images));
        write_unicode_wim_fixture(source, archive, images);
        const auto stats = superzip::extract_wim(archive, output, false);
        REQUIRE_EQ(stats.output_bytes, std::string_view("wim unicode payload\n").size() * images);
        for (int index = 1; index <= images; ++index) {
            const auto image_root = images == 1 ? output : output / ("image-" + std::to_string(index));
            REQUIRE_EQ(read_text(image_root / relative), "wim unicode payload\n");
            REQUIRE_TRUE(std::filesystem::is_directory(image_root / L"empty-\u03a9"));
            REQUIRE_EQ(std::filesystem::file_size(image_root / L"empty-\u00e9.txt"), 0U);
            std::ofstream(image_root / relative, std::ios::binary | std::ios::trunc) << "old";
        }
        bool refused = false;
        try {
            (void)superzip::extract_wim(archive, output, false);
        } catch (const superzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        for (int index = 1; index <= images; ++index) {
            const auto image_root = images == 1 ? output : output / ("image-" + std::to_string(index));
            REQUIRE_EQ(read_text(image_root / relative), "old");
        }
        (void)superzip::extract_wim(archive, output, true);
        for (int index = 1; index <= images; ++index) {
            const auto image_root = images == 1 ? output : output / ("image-" + std::to_string(index));
            REQUIRE_EQ(read_text(image_root / relative), "wim unicode payload\n");
        }
        for (const auto& entry : std::filesystem::directory_iterator(output)) {
            REQUIRE_TRUE(!entry.path().filename().wstring().starts_with(L".superzip"));
        }
        if (images == 2) {
            superzip_test::export_compat_fixture(archive, output);
        }
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(wim_extraction_refuses_to_overwrite_existing_files) {
    const auto root = test_temp_dir("wim-overwrite");
    const auto destination = root / "out";
    std::filesystem::create_directories(destination);
    {
        std::ofstream existing(destination / "root.txt", std::ios::binary);
        existing << "existing";
    }

    bool refused = false;
    try {
        (void)superzip::extract_wim(wim_fixture_path("basic.wim"), destination, false);
    } catch (const superzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text(destination / "root.txt"), std::string("existing"));
}

TEST_CASE(wim_extraction_rejects_truncated_archives) {
    const auto root = test_temp_dir("wim-truncated");
    const auto corrupt = root / "truncated.wim";
    std::filesystem::copy_file(wim_fixture_path("basic.wim"), corrupt);
    std::filesystem::resize_file(corrupt, 128U);

    bool rejected = false;
    try {
        (void)superzip::extract_wim(corrupt, root / "out", false);
    } catch (const superzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}

TEST_CASE(wim_extraction_rejects_split_wim_extension_until_multipart_support_exists) {
    const auto root = test_temp_dir("wim-swm-rejected");
    const auto split_part = root / "part.swm";
    std::filesystem::copy_file(wim_fixture_path("basic.wim"), split_part);

    REQUIRE_EQ(superzip::detect_archive_format(split_part), superzip::ArchiveFormat::SplitWim);
    bool rejected = false;
    try {
        (void)superzip::extract_wim(split_part, root / "out", false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}
