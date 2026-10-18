#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/file_publish.hpp"
#include "core/path_text.hpp"
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

#ifndef FLITZZIP_TEST_FIXTURES_DIR
#define FLITZZIP_TEST_FIXTURES_DIR "tests/fixtures"
#endif

// Purpose: Resolve a pinned WIM fixture path for tests.
// Inputs: `name` is the fixture filename under `tests/fixtures/wim`.
// Outputs: Returns the absolute or source-relative fixture path.
std::filesystem::path wim_fixture_path(const std::string& name) {
    return std::filesystem::path(FLITZZIP_TEST_FIXTURES_DIR) / "wim" / name;
}

// Purpose: Read a small text fixture result.
// Inputs: `path` is an extracted text file.
// Outputs: Returns the full text payload or throws on I/O failure.
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open text file: " + flitzzip::path_diagnostic_utf8(path));
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
// Inputs: `source` is a tiny test tree, `archive` is its output path, and `images` is within 1..65.
// Outputs: Writes an integrity-checked WIM without capturing ACLs; requires prior product runtime initialization.
void write_unicode_wim_fixture(const std::filesystem::path& source, const std::filesystem::path& archive, int images) {
    REQUIRE_TRUE(images >= 1 && images <= 65);
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

    REQUIRE_EQ(flitzzip::detect_archive_format(fixture), flitzzip::ArchiveFormat::Wim);
    const auto& info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Wim);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);

    const auto stats = flitzzip::extract_wim(fixture, destination, false);
    REQUIRE_TRUE(stats.entries >= 2U);
    REQUIRE_EQ(read_text(destination / "root.txt"), std::string("wim-root"));
    REQUIRE_EQ(read_text(destination / "nested" / "hello.txt"), std::string("hello wim"));
    flitzzip_test::export_compat_fixture(fixture, destination);
}

// Purpose: Preserve UTF-16 WIM names in private staging and single/multiple-image publication.
// Inputs: Tiny library-generated images with accented, CJK, supplementary, empty-file, and directory names.
// Outputs: Checks exact payloads and names, explicit overwrite, and private staging cleanup.
TEST_CASE(wim_unicode_names_extract_and_overwrite) {
    const auto root = test_temp_dir("wim-unicode") / L"\u65e5\u672c";
    std::filesystem::create_directories(root);
    (void)flitzzip::extract_wim(wim_fixture_path("basic.wim"), root / "initialize", false);
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
        const auto stats = flitzzip::extract_wim(archive, output, false);
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
            (void)flitzzip::extract_wim(archive, output, false);
        } catch (const flitzzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        for (int index = 1; index <= images; ++index) {
            const auto image_root = images == 1 ? output : output / ("image-" + std::to_string(index));
            REQUIRE_EQ(read_text(image_root / relative), "old");
        }
        (void)flitzzip::extract_wim(archive, output, true);
        for (int index = 1; index <= images; ++index) {
            const auto image_root = images == 1 ? output : output / ("image-" + std::to_string(index));
            REQUIRE_EQ(read_text(image_root / relative), "wim unicode payload\n");
        }
        for (const auto& entry : std::filesystem::directory_iterator(output)) {
            REQUIRE_TRUE(!entry.path().filename().wstring().starts_with(L".flitzzip"));
        }
        if (images == 2) {
            flitzzip_test::export_compat_fixture(archive, output);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Exercise nested WIM staging beyond legacy Win32 limits without machine-wide long-path opt-in.
// Inputs: A two-image Unicode fixture, long archive/destination paths, and both publication policies.
// Outputs: Checks exact names/bytes, empty entries, overwrite refusal/replacement, and complete staging cleanup.
TEST_CASE(wim_unicode_long_paths_preserve_publication_policy) {
    const auto root = test_temp_dir("wim-long-path");
    (void)flitzzip::extract_wim(wim_fixture_path("basic.wim"), root / "initialize", false);
    const auto source = root / "source";
    const auto relative = std::filesystem::path(L"caf\u00e9") / L"\u65e5\u672c-\U0001f680.txt";
    std::filesystem::create_directories((source / relative).parent_path());
    std::filesystem::create_directories(source / L"empty-\u03a9");
    std::ofstream(source / relative, std::ios::binary) << "long path payload";
    std::ofstream(source / L"empty-\u00e9.txt", std::ios::binary);
    auto long_root = root;
    while (long_root.native().size() < 320U)
        long_root /= std::wstring(60U, L'p');
    const auto long_io = std::filesystem::path(flitzzip::windows_api_path(long_root));
    std::filesystem::create_directories(long_io);
    write_unicode_wim_fixture(source, long_io / L"archive-\u03a9.wim", 2);
    for (const bool validate : {false, true}) {
        const auto output = long_root / (validate ? "validated" : "direct");
        const auto output_io = std::filesystem::path(flitzzip::windows_api_path(output));
        int calls = 0;
        std::filesystem::path stage;
        const auto extract = [&](bool overwrite) {
            flitzzip::extract_with_publication(
                {.destination = output, .overwrite = overwrite, .validate_before_publish = validate},
                [&](const auto& destination, bool replace) {
                    ++calls;
                    stage = destination;
                    const auto stats = flitzzip::extract_wim(long_root / L"archive-\u03a9.wim", destination, replace);
                    REQUIRE_EQ(stats.output_bytes, 2U * std::string_view("long path payload").size());
                });
        };
        extract(false);
        REQUIRE_EQ(calls, 1);
        if (validate)
            REQUIRE_TRUE(!std::filesystem::exists(stage));
        for (const auto* image : {"image-1", "image-2"}) {
            REQUIRE_EQ(read_text(output_io / image / relative), "long path payload");
            REQUIRE_TRUE(std::filesystem::is_directory(output_io / image / L"empty-\u03a9"));
            REQUIRE_EQ(std::filesystem::file_size(output_io / image / L"empty-\u00e9.txt"), 0U);
            std::ofstream(output_io / image / relative, std::ios::binary | std::ios::trunc) << "old";
        }
        bool refused = false;
        try {
            extract(false);
        } catch (const flitzzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        REQUIRE_EQ(calls, 2);
        if (validate)
            REQUIRE_TRUE(!std::filesystem::exists(stage));
        for (const auto* image : {"image-1", "image-2"})
            REQUIRE_EQ(read_text(output_io / image / relative), "old");
        extract(true);
        REQUIRE_EQ(calls, 3);
        for (const auto* image : {"image-1", "image-2"})
            REQUIRE_EQ(read_text(output_io / image / relative), "long path payload");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(output_io))
            REQUIRE_TRUE(!entry.path().filename().wstring().starts_with(L".flitzzip"));
    }
    std::filesystem::remove_all(std::filesystem::path(flitzzip::windows_api_path(root)));
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
        (void)flitzzip::extract_wim(wim_fixture_path("basic.wim"), destination, false);
    } catch (const flitzzip::SecurityError&) {
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
        (void)flitzzip::extract_wim(corrupt, root / "out", false);
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}

TEST_CASE(wim_extraction_rejects_split_wim_extension_until_multipart_support_exists) {
    const auto root = test_temp_dir("wim-swm-rejected");
    const auto split_part = root / "part.swm";
    std::filesystem::copy_file(wim_fixture_path("basic.wim"), split_part);

    REQUIRE_EQ(flitzzip::detect_archive_format(split_part), flitzzip::ArchiveFormat::SplitWim);
    bool rejected = false;
    try {
        (void)flitzzip::extract_wim(split_part, root / "out", false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}

namespace {

// Purpose: Prepare a tiny independent WIM source and admit the same pinned runtime used by production.
// Inputs: Owned fixture root. Outputs: Source directory containing one known payload; no unrelated files change.
std::filesystem::path wim_failure_source(const std::filesystem::path& root) {
    (void)flitzzip::extract_wim(wim_fixture_path("basic.wim"), root / "initialize", false);
    const auto source = root / "source";
    std::filesystem::create_directory(source);
    std::ofstream payload(source / "payload.txt", std::ios::binary);
    payload << "Independent WIM boundary payload.";
    payload.close();
    REQUIRE_TRUE(payload.good());
    return source;
}

// Purpose: Require metadata rejection before publication while preserving existing caller data.
// Inputs: Independent WIM, owned destination and the expected production error category text.
// Outputs: Requires the exact rejection, unchanged sentinel and no additional destination entries or staging debris.
void require_wim_metadata_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                   std::string_view expected) {
    std::filesystem::create_directory(destination);
    {
        std::ofstream sentinel(destination / "sentinel.txt", std::ios::binary);
        sentinel << "preserve caller data";
    }
    bool rejected = false;
    try {
        (void)flitzzip::extract_wim(archive, destination, true);
    } catch (const flitzzip::Error& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(expected) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text(destination / "sentinel.txt"), std::string("preserve caller data"));
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
               1);
}
}  // namespace

// Purpose: Exercise the maximum supported image count and reject one further valid independent image.
// Inputs: Library-written uncompressed WIMs with one small file in each image.
// Outputs: All 64 images recover exactly; 65 images reject before changing destination contents.
TEST_CASE(wim_image_count_boundary_preserves_each_image) {
    const auto root = test_temp_dir("wim-image-boundary");
    const auto source = wim_failure_source(root);
    const auto accepted = root / "accepted.wim";
    write_unicode_wim_fixture(source, accepted, 64);
    const auto destination = root / "accepted";
    const auto stats = flitzzip::extract_wim(accepted, destination, false);
    REQUIRE_EQ(stats.entries, 64U);
    for (unsigned image = 1; image <= 64; ++image) {
        REQUIRE_EQ(read_text(destination / ("image-" + std::to_string(image)) / "payload.txt"),
                   std::string("Independent WIM boundary payload."));
    }
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
               64);
    const auto rejected = root / "rejected.wim";
    write_unicode_wim_fixture(source, rejected, 65);
    require_wim_metadata_rejected(rejected, root / "rejected", "image count exceeds");
    REQUIRE_TRUE(std::filesystem::file_size(accepted) + std::filesystem::file_size(rejected) < 4U * 1024U * 1024U);
}

// Purpose: Reject a valid image that has no publishable entries.
// Inputs: A library-written WIM containing only the root directory.
// Outputs: Explicit empty-image error without staging or replacing existing destination data.
TEST_CASE(wim_empty_image_rejects_without_publication) {
    const auto root = test_temp_dir("wim-empty-image");
    const auto source = wim_failure_source(root);
    REQUIRE_TRUE(std::filesystem::remove(source / "payload.txt"));
    const auto archive = root / "empty.wim";
    write_unicode_wim_fixture(source, archive, 1);
    require_wim_metadata_rejected(archive, root / "out", "no extractable entries");
}

// Purpose: Keep captured hard links outside the standalone WIM extraction contract.
// Inputs: Two real same-volume names for one file, captured by the independent WIM writer.
// Outputs: Explicit hard-link rejection before destination publication.
TEST_CASE(wim_hard_link_metadata_rejects_without_publication) {
    const auto root = test_temp_dir("wim-hard-links");
    const auto source = wim_failure_source(root);
    std::filesystem::create_hard_link(source / "payload.txt", source / "linked.txt");
    REQUIRE_TRUE(std::filesystem::equivalent(source / "payload.txt", source / "linked.txt"));
    const auto archive = root / "links.wim";
    write_unicode_wim_fixture(source, archive, 1);
    require_wim_metadata_rejected(archive, root / "out", "hard links are not supported");
}

// Purpose: Reject named NTFS streams instead of silently dropping or publishing them.
// Inputs: A real owned file with an independent alternate stream, captured by the WIM writer.
// Outputs: Explicit stream rejection and unchanged destination data.
TEST_CASE(wim_named_stream_metadata_rejects_without_publication) {
    const auto root = test_temp_dir("wim-named-stream");
    const auto source = wim_failure_source(root);
    const auto stream = source / "payload.txt:hidden";
    {
        std::ofstream output(stream, std::ios::binary);
        output << "named stream payload";
        output.close();
        REQUIRE_TRUE(output.good());
    }
    REQUIRE_EQ(read_text(stream), std::string("named stream payload"));
    const auto archive = root / "stream.wim";
    write_unicode_wim_fixture(source, archive, 1);
    require_wim_metadata_rejected(archive, root / "out", "alternate data streams are not supported");
}

// Purpose: Preserve callback cancellation through staged extraction and permit a clean subsequent attempt.
// Inputs: Valid WIM and a callback throwing before the first entry is published.
// Outputs: Exact callback error, unchanged old payload, no staging debris, then successful byte-exact reuse.
TEST_CASE(wim_cancellation_cleans_staging_and_preserves_destination) {
    const auto root = test_temp_dir("wim-cancel");
    const auto source = wim_failure_source(root);
    const auto archive = root / "input.wim";
    write_unicode_wim_fixture(source, archive, 1);
    const auto destination = root / "out";
    std::filesystem::create_directory(destination);
    std::ofstream(destination / "payload.txt", std::ios::binary) << "original payload";
    unsigned callbacks = 0;
    bool cancelled = false;
    try {
        (void)flitzzip::extract_wim(archive, destination, true, [&](const flitzzip::ProgressSnapshot&) {
            ++callbacks;
            throw std::runtime_error("independent WIM cancellation");
        });
    } catch (const std::runtime_error& error) {
        REQUIRE_EQ(std::string(error.what()), std::string("independent WIM cancellation"));
        cancelled = true;
    }
    REQUIRE_TRUE(cancelled);
    REQUIRE_EQ(callbacks, 1U);
    REQUIRE_EQ(read_text(destination / "payload.txt"), std::string("original payload"));
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
               1);
    const auto stats = flitzzip::extract_wim(archive, destination, true);
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(read_text(destination / "payload.txt"), std::string("Independent WIM boundary payload."));
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
               1);
}
