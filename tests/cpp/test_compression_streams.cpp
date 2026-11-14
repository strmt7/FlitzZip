#include "test_util.hpp"
#include "test_stream_failure.hpp"
#include "test_source_session.hpp"
#include "test_stream_progress.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "bzip2/bzip2_adapter.hpp"
#include "base64/base64_adapter.hpp"
#include "core/result.hpp"
#include "core/decoded_input_buffer.hpp"
#include "core/stream_byte_consumer.hpp"
#include "core/archive_stream_progress.hpp"
#include "tar/tar_adapter.hpp"
#include "cpio/cpio_adapter.hpp"
#include "core/path_text.hpp"
#include "core/bounded_text_io.hpp"
#include "core/archive_input_progress.hpp"
#include "gzip/gzip_stream.hpp"
#include "gzip/gzip_adapter.hpp"
#include "lzma/bounded_sdk_allocator.hpp"
#include "miniz.h"
#include "xz/xz_stream.hpp"
#include "zstd/zstd_stream.hpp"
#include "zstd/zstd_adapter.hpp"
#include "zstd/zstd_runtime.hpp"
#include "zip/zip_adapter.hpp"
#include "uue/uue_adapter.hpp"
#include "xxe/xxe_adapter.hpp"
#include "test_flzip_helpers.hpp"

#include <array>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <memory>
#include <thread>
#include <sstream>
#include <utility>

namespace {

enum class CreatedSourceFormat { Flzip, Zip, Base64, Uue, Xxe };

struct ContainerProgressAbort {};

struct ContainerProgressCase {
    const char* extension;
    bool filtered;
    void (*create)(const std::filesystem::path&, const std::filesystem::path&);
    flitzzip_test::ArchiveExtractor extract;
};

// Use existing production writers; this is a fixed correctness fixture, not a
// corpus or timing workload. Callback failures are observed through public APIs.
std::array<ContainerProgressCase, 6> container_progress_cases() {
    using Path = std::filesystem::path;
    return {{
        {"tar", false, [](const Path& source, const Path& archive) { (void)flitzzip::compress_tar({source}, archive); },
         &flitzzip::extract_tar},
        {"tar.gz", true,
         [](const Path& source, const Path& archive) { (void)flitzzip::compress_tar_gzip({source}, archive); },
         &flitzzip::extract_tar_gzip},
        {"tar.bz2", true,
         [](const Path& source, const Path& archive) { (void)flitzzip::compress_tar_bzip2({source}, archive); },
         &flitzzip::extract_tar_bzip2},
        {"tar.zst", true,
         [](const Path& source, const Path& archive) { (void)flitzzip::compress_tar_zstd({source}, archive); },
         &flitzzip::extract_tar_zstd},
        {"cpio", false,
         [](const Path& source, const Path& archive) { (void)flitzzip::compress_cpio({source}, archive); },
         &flitzzip::extract_cpio},
        {"cpio.gz", true,
         [](const Path& source, const Path& archive) { (void)flitzzip::compress_cpio_gzip({source}, archive); },
         &flitzzip::extract_cpio_gzip},
    }};
}

// Purpose: Select an explicitly CPU-only, small-memory native correctness workload without changing product defaults.
// Inputs: overwrite controls only the owned fixture's target replacement. Outputs: Standard-mode bounded decode
// options.
flitzzip::ExtractOptions source_session_cpu_options(bool overwrite) {
    flitzzip::ExtractOptions options;
    options.gpu_required = false;
    options.force_cpu = true;
    options.overwrite = overwrite;
    options.chunk_size = 64U * 1024U;
    options.block_size = 4U * 1024U;
    options.worker_count = 1U;
    options.max_inflight_chunks = 1U;
    return options;
}

// Purpose: Exercise raw native/compatibility APIs without inheriting the CLI's separate source session.
// Inputs: An owned standard-mode format and whether to exercise its supported read-only verifier.
// Outputs: Exact fixture bytes, real write exclusion, typed failure cleanup and successful source reuse.
void require_created_archive_source_session(CreatedSourceFormat format, bool verification) {
    const auto root = test_temp_dir("created-source-session-" + std::to_string(static_cast<int>(format)) +
                                    (verification ? "-verify" : "-extract"));
    const auto source = root / "payload";
    const std::string expected = "public archive readers must retain one immutable source identity\n";
    {
        std::ofstream output(source, std::ios::binary);
        output << expected;
        REQUIRE_TRUE(output.good());
    }
    auto archive = root / "payload";
    flitzzip_test::ArchiveExtractor extractor = nullptr;
    switch (format) {
    case CreatedSourceFormat::Flzip: {
        archive += ".flzip";
        const auto cpu = source_session_cpu_options(false);
        flitzzip::CompressOptions options;
        options.gpu_required = false;
        options.force_cpu = true;
        options.chunk_size = cpu.chunk_size;
        options.block_size = cpu.block_size;
        options.worker_count = cpu.worker_count;
        options.max_inflight_chunks = cpu.max_inflight_chunks;
        options.verify_after_write = true;
        REQUIRE_EQ(flitzzip::compress_flzip({source}, archive, options).input_bytes, expected.size());
        extractor = [](const std::filesystem::path& input, const std::filesystem::path& output, bool overwrite,
                       const flitzzip::ProgressCallback& callback) {
            return flitzzip::extract_flzip(input, output, source_session_cpu_options(overwrite), callback);
        };
        break;
    }
    case CreatedSourceFormat::Zip:
        archive += ".zip";
        (void)flitzzip::compress_zip({source}, archive, 5, {}, true);
        extractor = &flitzzip::extract_zip;
        break;
    case CreatedSourceFormat::Base64:
        archive += ".b64";
        (void)flitzzip::compress_base64_file(source, archive);
        extractor = &flitzzip::extract_base64_file;
        break;
    case CreatedSourceFormat::Uue:
        archive += ".uue";
        (void)flitzzip::compress_uue_file(source, archive);
        extractor = &flitzzip::extract_uue_file;
        break;
    case CreatedSourceFormat::Xxe:
        archive += ".xxe";
        (void)flitzzip::compress_xxe_file(source, archive);
        extractor = &flitzzip::extract_xxe_file;
        break;
    }
    REQUIRE_TRUE(extractor != nullptr);
    if (verification) {
        REQUIRE_TRUE(format == CreatedSourceFormat::Zip || format == CreatedSourceFormat::Flzip);
        flitzzip_test::require_archive_verification_source_session(
            archive, expected.size(), [&](const flitzzip::ProgressCallback& callback) {
                return format == CreatedSourceFormat::Zip
                           ? flitzzip::verify_zip(archive, callback)
                           : flitzzip::verify_flzip(archive, source_session_cpu_options(false), callback);
            });
    } else {
        flitzzip_test::require_archive_source_session(archive, root, "payload", expected, extractor);
    }
}

// Purpose: Prepare a real compatibility stream for shared public extraction ownership assertions.
// Inputs: One codec selector and a unique owned fixture label; expected plaintext remains independently retained.
// Outputs: A complete reader lifetime, admission and recovery check for the selected actual format.
void require_compressed_source_session(int codec, const char* name) {
    const auto root = test_temp_dir(name);
    const auto source = root / "payload";
    const std::string expected = "source ownership must survive decoder work and callback unwinding\n";
    {
        std::ofstream output(source, std::ios::binary);
        output << expected;
        REQUIRE_TRUE(output.good());
    }
    const auto archive = root / (codec == 0 ? "payload.gz" : codec == 1 ? "payload.bz2" : "payload.zst");
    flitzzip_test::ArchiveExtractor extractor = nullptr;
    if (codec == 0) {
        (void)flitzzip::compress_gzip_file(source, archive);
        extractor = &flitzzip::extract_gzip_file;
    } else if (codec == 1) {
        (void)flitzzip::compress_bzip2_file(source, archive);
        extractor = &flitzzip::extract_bzip2_file;
    } else {
        (void)flitzzip::compress_zstd({source}, archive);
        extractor = &flitzzip::extract_zstd_file;
    }
    flitzzip_test::require_archive_source_session(archive, root, "payload", expected, extractor);
}

}  // namespace

namespace {

// Fixed multi-window input tests callback/cancellation ownership, not compression
// size or timing. The source bytes supply an independent extraction oracle.
template <class Writer>
void require_adapter_window_cancellation(const char* name, const char* extension,
                                         flitzzip_test::ArchiveExtractor extract) {
    const auto root = test_temp_dir(name);
    const auto archive = root / (std::string("payload") + extension);
    std::string expected(3U * 65536U + 17U, '\0');
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        expected[index] = static_cast<char>((index * 37U + index / 251U) & 0xFFU);
    }
    {
        Writer output(archive, 5);
        output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        output.close();
    }
    flitzzip_test::require_stream_window_cancellation(archive, root / "out", "payload", expected, extract);
}

}  // namespace

TEST_CASE(gzip_extract_cancellation_between_output_windows) {
    require_adapter_window_cancellation<flitzzip::GzipOutputStream>("gzip-window-cancellation", ".gz",
                                                                    &flitzzip::extract_gzip_file);
}

TEST_CASE(bzip2_extract_cancellation_between_output_windows) {
    require_adapter_window_cancellation<flitzzip::Bzip2OutputStream>("bzip2-window-cancellation", ".bz2",
                                                                     &flitzzip::extract_bzip2_file);
}

TEST_CASE(zstd_extract_cancellation_between_output_windows) {
    require_adapter_window_cancellation<flitzzip::ZstdOutputStream>("zstd-window-cancellation", ".zst",
                                                                    &flitzzip::extract_zstd_file);
}

// Purpose: Check native source lifetime. Inputs: A tiny CPU fixture. Outputs: Exact extraction and lock recovery.
TEST_CASE(archive_source_session_flzip) {
    require_created_archive_source_session(CreatedSourceFormat::Flzip, false);
}

// Purpose: Check ZIP source lifetime. Inputs: A tiny compatibility fixture. Outputs: Exact extraction and lock
// recovery.
TEST_CASE(archive_source_session_zip) {
    require_created_archive_source_session(CreatedSourceFormat::Zip, false);
}

// Purpose: Check Base64 source lifetime. Inputs: A tiny text fixture. Outputs: Exact extraction and lock recovery.
TEST_CASE(archive_source_session_base64) {
    require_created_archive_source_session(CreatedSourceFormat::Base64, false);
}

// Purpose: Check UUencode source lifetime. Inputs: A tiny text fixture. Outputs: Exact extraction and lock recovery.
TEST_CASE(archive_source_session_uue) {
    require_created_archive_source_session(CreatedSourceFormat::Uue, false);
}

// Purpose: Check XXencode source lifetime. Inputs: A tiny text fixture. Outputs: Exact extraction and lock recovery.
TEST_CASE(archive_source_session_xxe) {
    require_created_archive_source_session(CreatedSourceFormat::Xxe, false);
}

// Purpose: Check native verification lifetime. Inputs: A tiny CPU fixture. Outputs: Read-only lock and failure
// recovery.
TEST_CASE(archive_source_session_verify_flzip) {
    require_created_archive_source_session(CreatedSourceFormat::Flzip, true);
}

// Purpose: Check ZIP verification lifetime. Inputs: A tiny verified fixture. Outputs: Read-only lock and failure
// recovery.
TEST_CASE(archive_source_session_verify_zip) {
    require_created_archive_source_session(CreatedSourceFormat::Zip, true);
}

// Purpose: Check Gzip source admission/lifetime. Inputs: A small real stream. Outputs: Exact recovery and sharing
// proof.
TEST_CASE(single_stream_source_session_gzip) {
    require_compressed_source_session(0, "gzip-source-session");
}

// Purpose: Check Bzip2 source admission/lifetime. Inputs: A small real stream. Outputs: Exact recovery and sharing
// proof.
TEST_CASE(single_stream_source_session_bzip2) {
    require_compressed_source_session(1, "bzip2-source-session");
}

// Purpose: Check Zstandard source lifetime. Inputs: A small real stream. Outputs: Exact recovery and sharing proof.
TEST_CASE(single_stream_source_session_zstandard) {
    require_compressed_source_session(2, "zstandard-source-session");
}

// Purpose: Preserve the original file-size failure when compressed-stream filenames cannot use the host code page.
// Inputs: A missing Unicode/supplementary filename through Gzip, Bzip2, Zstandard and XZ readers.
// Outputs: Requires ArchiveError with the intact UTF-8 filename, not an unrelated narrowing exception.
TEST_CASE(compression_stream_missing_unicode_file_diagnostics) {
    const auto root = test_temp_dir("compression-stream-missing-unicode");
    const auto missing = root / std::filesystem::path(u8"\u538b\u7f29-\U0001f4e6.bin");
    const auto filename = flitzzip::path_diagnostic_utf8(missing.filename());
    for (int codec = 0; codec < 4; ++codec) {
        bool rejected = false;
        try {
            if (codec == 0) {
                flitzzip::GzipInputStream input(missing);
            } else if (codec == 1) {
                flitzzip::Bzip2InputStream input(missing);
            } else if (codec == 2) {
                flitzzip::ZstdInputStream input(missing);
            } else {
                flitzzip::XzInputStream input(missing);
            }
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(filename) != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Preserve useful creation errors when a Unicode output has no parent directory.
// Inputs: The same nonexistent Unicode/supplementary output path through all three stream writers.
// Outputs: Requires ArchiveError containing the original UTF-8 filename and leaves the parent absent.
TEST_CASE(compression_stream_unicode_create_failure_diagnostics) {
    const auto root = test_temp_dir("compression-stream-unicode-create-failure");
    const auto missing = root / "missing-parent" / std::filesystem::path(u8"\u538b\u7f29-\U0001f4e6.bin");
    const auto filename = flitzzip::path_diagnostic_utf8(missing.filename());
    for (int codec = 0; codec < 3; ++codec) {
        bool rejected = false;
        try {
            if (codec == 0) {
                flitzzip::GzipOutputStream output(missing, 5);
            } else if (codec == 1) {
                flitzzip::Bzip2OutputStream output(missing, 5);
            } else {
                flitzzip::ZstdOutputStream output(missing, 5);
            }
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(filename) != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(!std::filesystem::exists(missing.parent_path()));
    }
}

#ifdef _WIN32
// Purpose: Preserve actionable source-admission refusal for a Unicode file held against readers.
// Inputs: One temporary, exclusively held Windows file with a Unicode/supplementary filename.
// Outputs: Requires intact UTF-8 SecurityError diagnostics and the actual sharing error from all three readers.
TEST_CASE(compression_stream_unicode_locked_file_diagnostics) {
    const auto root = test_temp_dir("compression-stream-unicode-locked-file");
    const auto path = root / std::filesystem::path(u8"\u538b\u7f29-\U0001f4e6.bin");
    {
        std::ofstream seed(path, std::ios::binary);
        seed.put('x');
        REQUIRE_TRUE(seed.good());
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
    const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
    const auto filename = flitzzip::path_diagnostic_utf8(path.filename());
    for (int codec = 0; codec < 3; ++codec) {
        bool rejected = false;
        try {
            if (codec == 0) {
                flitzzip::GzipInputStream input(path);
            } else if (codec == 1) {
                flitzzip::Bzip2InputStream input(path);
            } else {
                flitzzip::ZstdInputStream input(path);
            }
        } catch (const flitzzip::SecurityError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(filename) != std::string_view::npos);
            REQUIRE_TRUE(std::string_view(error.what()).find("Windows error 32") != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}
#endif

// Purpose: Exercise the actual SDK callbacks at exact limits without allocating large hostile dictionaries.
// Inputs: Two independent allocators, zero-size requests, exhausted budgets, and released pointers.
// Outputs: Requires zero initialization, isolated accounting, refusal before overflow, and reusable capacity.
TEST_CASE(sdk_allocator_enforces_owner_limits_through_c_callbacks) {
    flitzzip::BoundedSdkAllocator first(16);
    flitzzip::BoundedSdkAllocator second(8);
    auto* first_bytes = static_cast<unsigned char*>(ISzAlloc_Alloc(&first, 16));
    REQUIRE_TRUE(first_bytes != nullptr);
    for (std::size_t index = 0; index < 16; ++index) {
        REQUIRE_EQ(first_bytes[index], 0U);
    }
    REQUIRE_EQ(first.current_bytes(), 16U);
    REQUIRE_TRUE(ISzAlloc_Alloc(&first, 1) == nullptr);
    auto* second_bytes = ISzAlloc_Alloc(&second, 8);
    REQUIRE_TRUE(second_bytes != nullptr);
    ISzAlloc_Free(&second, first_bytes);
    REQUIRE_EQ(first.current_bytes(), 16U);
    REQUIRE_EQ(second.current_bytes(), 8U);
    ISzAlloc_Free(&first, first_bytes);
    REQUIRE_EQ(first.current_bytes(), 0U);
    auto* zero = ISzAlloc_Alloc(&first, 0);
    REQUIRE_TRUE(zero != nullptr);
    REQUIRE_EQ(first.current_bytes(), 1U);
    ISzAlloc_Free(&first, zero);
    ISzAlloc_Free(&first, nullptr);
    ISzAlloc_Free(&second, second_bytes);
    REQUIRE_EQ(second.current_bytes(), 0U);
    REQUIRE_TRUE(ISzAlloc_Alloc(&first, std::numeric_limits<std::size_t>::max()) == nullptr);
    flitzzip::BoundedSdkAllocator empty(0);
    REQUIRE_TRUE(ISzAlloc_Alloc(&empty, 0) == nullptr);
}

// Purpose: Preserve allocation ownership across interleaved owners and a sequential decoder thread handoff.
// Inputs: A callback allocation created on one thread and freed on another while another owner stays live.
// Outputs: Requires no ambient-thread dependency, exact retained accounting, and independent subsequent allocation.
TEST_CASE(sdk_allocator_context_survives_sequential_thread_handoff) {
    flitzzip::BoundedSdkAllocator first(32);
    flitzzip::BoundedSdkAllocator second(32);
    auto* retained = ISzAlloc_Alloc(&second, 16);
    REQUIRE_TRUE(retained != nullptr);
    void* handed_off = nullptr;
    std::thread allocate([&] { handed_off = ISzAlloc_Alloc(&first, 32); });
    allocate.join();
    REQUIRE_TRUE(handed_off != nullptr);
    REQUIRE_EQ(first.current_bytes(), 32U);
    std::thread release([&] { ISzAlloc_Free(&first, handed_off); });
    release.join();
    REQUIRE_EQ(first.current_bytes(), 0U);
    REQUIRE_EQ(second.current_bytes(), 16U);
    auto* replacement = ISzAlloc_Alloc(&first, 32);
    REQUIRE_TRUE(replacement != nullptr);
    ISzAlloc_Free(&first, replacement);
    ISzAlloc_Free(&second, retained);
}

// Purpose: Preserve host Unicode archive filenames through each writable single-stream adapter.
// Inputs: Three production adapters, uppercase suffix aliases, and a bounded binary payload.
// Outputs: Requires byte-exact extraction to the native filename and non-destructive overwrite refusal.
TEST_CASE(single_stream_unicode_archive_names_roundtrip) {
    using Compress = flitzzip::OperationStats (*)(const std::vector<std::filesystem::path>&,
                                                  const std::filesystem::path&, int, const flitzzip::ProgressCallback&);
    using Extract = flitzzip::OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&, bool,
                                                 const flitzzip::ProgressCallback&);
    struct Case {
        const char* suffix;
        Compress compress;
        Extract extract;
    };
    const std::array cases = {
        Case{".GZ", flitzzip::compress_gzip, flitzzip::extract_gzip_file},
        Case{".BZ2", flitzzip::compress_bzip2, flitzzip::extract_bzip2_file},
        Case{".ZST", flitzzip::compress_zstd, flitzzip::extract_zstd_file},
        Case{".ZSTD", flitzzip::compress_zstd, flitzzip::extract_zstd_file},
    };
    const auto root = test_temp_dir("single-stream-unicode-names");
    const auto filename = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.bin");
    const std::string payload("binary\0payload\xff", 15);
    const auto source = root / "source" / filename;
    std::filesystem::create_directory(source.parent_path());
    {
        std::ofstream output(source, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    for (const auto& item : cases) {
        auto archive = root / filename;
        archive += item.suffix;
        (void)item.compress({source}, archive, 5, {});
        const auto destination = root / (std::string("restore") + item.suffix);
        (void)item.extract(archive, destination, false, {});
        const auto target = destination / filename;
        REQUIRE_TRUE(std::filesystem::exists(target));
        std::ifstream input(target, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), payload);
        bool refused = false;
        try {
            (void)item.extract(archive, destination, false, {});
        } catch (const flitzzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        REQUIRE_EQ(std::filesystem::file_size(target), payload.size());
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve bounded archive text-line behavior across LF, CRLF, empty, binary, and unterminated lines.
// Inputs: An in-memory stream containing boundary-length lines and embedded NUL bytes.
// Outputs: Requires exact content, clean EOF, and rejection before an overlong line grows past its limit.
TEST_CASE(bounded_archive_text_lines_preserve_boundaries_and_eof) {
    std::istringstream input(std::string("abc\nxy\r\n\n") + std::string("\0z", 2) + "\nend\r");
    std::string line;
    for (const auto& expected : std::array<std::string, 5>{"abc", "xy", "", std::string("\0z", 2), "end"}) {
        REQUIRE_TRUE(flitzzip::read_bounded_text_line(input, line, 4, "Test"));
        REQUIRE_EQ(line, expected);
    }
    REQUIRE_TRUE(!flitzzip::read_bounded_text_line(input, line, 4, "Test"));
    REQUIRE_TRUE(line.empty());
    std::istringstream exact("1234\n");
    REQUIRE_TRUE(flitzzip::read_bounded_text_line(exact, line, 4, "Test"));
    REQUIRE_EQ(line, "1234");
    std::istringstream oversized("12345\n");
    bool rejected = false;
    try {
        (void)flitzzip::read_bounded_text_line(oversized, line, 4, "Test");
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string(error.what()) == "Test line exceeds FlitzZip metadata limit";
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(line.size(), 4U);
}

// Purpose: Distinguish failed text streams from clean EOF and preserve exact archive-line writes.
// Inputs: In-memory streams with failbit/badbit and a line containing an embedded NUL.
// Outputs: Requires stable read/write diagnostics and exact payload-plus-LF serialization.
TEST_CASE(encoded_input_progress_counts_framing_and_unterminated_lines) {
    std::istringstream input("A\r\n\nBC\rD");
    flitzzip::ProgressState progress;
    progress.start(flitzzip::OperationKind::Extract, 11U, 1U);
    std::size_t callbacks = 0;
    const flitzzip::ProgressCallback observer = [&](const auto& snapshot) {
        REQUIRE_EQ(snapshot.processed_bytes, 11U);
        ++callbacks;
    };
    flitzzip::ArchiveInputProgress work(progress, std::cref(observer), 11U, 3U);
    std::string line;
    for (const std::string expected : {"A", "", "BC\rD"}) {
        REQUIRE_TRUE(work.read_line(input, line, 4U, "Test"));
        REQUIRE_EQ(line, expected);
    }
    REQUIRE_TRUE(!work.read_line(input, line, 4U, "Test"));
    REQUIRE_EQ(callbacks, 0U);
    work.finish();
    REQUIRE_EQ(callbacks, 1U);
    REQUIRE_EQ(progress.snapshot().processed_bytes, 11U);
}

TEST_CASE(encoded_input_progress_preserves_stateful_observer_and_exception) {
    static_assert(!std::is_constructible_v<flitzzip::ArchiveInputProgress, flitzzip::ProgressState&,
                                           flitzzip::ProgressCallback&&, std::uint64_t>);
    static_assert(!std::is_constructible_v<flitzzip::ArchiveInputProgress, flitzzip::ProgressState&,
                                           const flitzzip::ProgressCallback&&, std::uint64_t>);
    flitzzip::ProgressState progress;
    progress.start(flitzzip::OperationKind::Extract, 131072U, 1U);
    struct Observer {
        std::size_t calls = 0;
        void operator()(const flitzzip::ProgressSnapshot& snapshot) {
            REQUIRE_EQ(snapshot.processed_bytes, 65536U);
            ++calls;
            throw std::range_error("original encoded-input observer");
        }
    };
    const flitzzip::ProgressCallback observer = Observer{};
    flitzzip::ArchiveInputProgress work(progress, std::cref(observer), 131072U);
    std::istringstream input(std::string(131072U, 'x'));
    bool interrupted = false;
    try {
        while (work.read_byte(input) != std::char_traits<char>::eof()) {
        }
    } catch (const std::range_error& error) {
        interrupted = std::string_view(error.what()) == "original encoded-input observer";
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_TRUE(observer.target<Observer>() != nullptr);
    REQUIRE_EQ(observer.target<Observer>()->calls, 1U);
    REQUIRE_EQ(progress.snapshot().processed_bytes, 65536U);
}

TEST_CASE(encoded_input_progress_rejects_inexact_extents_without_wrapping) {
    flitzzip::ProgressState progress;
    progress.start(flitzzip::OperationKind::Extract, 2U, 1U);
    const flitzzip::ProgressCallback no_observer;
    bool rejected = false;
    try {
        flitzzip::ArchiveInputProgress invalid(progress, std::cref(no_observer), 2U, 3U);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    flitzzip::ArchiveInputProgress work(progress, std::cref(no_observer), 2U);
    work.advance(1U);
    rejected = false;
    try {
        work.finish();
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    rejected = false;
    try {
        work.advance(std::numeric_limits<std::uint64_t>::max());
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    work.advance(1U);
    work.finish();
    REQUIRE_EQ(progress.snapshot().processed_bytes, 2U);
}

TEST_CASE(bounded_archive_text_io_reports_stream_failures) {
    for (const auto state : {std::ios::failbit, std::ios::badbit}) {
        std::istringstream input("data");
        input.setstate(state);
        std::string line;
        bool rejected = false;
        try {
            (void)flitzzip::read_bounded_text_line(input, line, 4, "Test");
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string(error.what()) == "failed to read Test stream";
        }
        REQUIRE_TRUE(rejected);
    }
    std::ostringstream output;
    const std::string line("a\0b", 3);
    flitzzip::write_archive_text_line(output, line, "Test");
    REQUIRE_EQ(output.str(), line + "\n");
    output.setstate(std::ios::badbit);
    bool rejected = false;
    try {
        flitzzip::write_archive_text_line(output, line, "Test");
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string(error.what()) == "failed to write Test stream";
    }
    REQUIRE_TRUE(rejected);
}

namespace {

// Purpose: Exercise miniz block selection with bounded output windows and automatic cleanup.
// Inputs: In-memory bytes, codec effort/strategy, and the maximum output bytes offered per call.
// Outputs: Returns a completed zlib stream; requires forward progress and successful codec finalization.
std::vector<unsigned char> encode_miniz_candidate(const std::string& input, int level, int strategy,
                                                  std::size_t output_window) {
    mz_stream stream{};
    REQUIRE_EQ(mz_deflateInit2(&stream, level, MZ_DEFLATED, MZ_DEFAULT_WINDOW_BITS, 8, strategy), MZ_OK);
    std::unique_ptr<mz_stream, decltype(&mz_deflateEnd)> cleanup(&stream, &mz_deflateEnd);
    std::vector<unsigned char> output(mz_compressBound(static_cast<mz_ulong>(input.size())));
    stream.next_in = reinterpret_cast<const unsigned char*>(input.data());
    stream.avail_in = static_cast<unsigned int>(input.size());
    for (;;) {
        REQUIRE_TRUE(stream.total_out < output.size());
        stream.next_out = output.data() + stream.total_out;
        stream.avail_out = static_cast<unsigned int>(std::min(output.size() - stream.total_out, output_window));
        const auto previous_output = stream.total_out;
        const auto status = mz_deflate(&stream, MZ_FINISH);
        if (status == MZ_STREAM_END) {
            break;
        }
        REQUIRE_EQ(status, MZ_OK);
        REQUIRE_TRUE(stream.total_out > previous_output);
    }
    REQUIRE_EQ(stream.total_in, input.size());
    output.resize(stream.total_out);
    return output;
}

// Purpose: Restore a generated miniz stream into exactly the expected bounded fixture size.
// Inputs: A complete zlib stream and its known original bytes.
// Outputs: Requires successful decompression, exact length, and byte equality.
void require_miniz_candidate_roundtrip(const std::vector<unsigned char>& encoded, const std::string& expected) {
    std::string decoded(expected.size(), '\0');
    auto size = static_cast<mz_ulong>(decoded.size());
    REQUIRE_EQ(mz_uncompress(reinterpret_cast<unsigned char*>(decoded.data()), &size, encoded.data(),
                             static_cast<mz_ulong>(encoded.size())),
               MZ_OK);
    REQUIRE_EQ(size, expected.size());
    REQUIRE_EQ(decoded, expected);
}

// Purpose: Read complete bounded fixture bytes and reject missing or unreadable fixture files.
// Inputs: Path to a test-owned small file.
// Outputs: Returns file contents or throws on open/read failure.
std::string read_stream_fixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_TRUE(file.is_open());
    file.exceptions(std::ios::badbit);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Purpose: Verify invalid effort is rejected before any destination is opened or truncated.
// Inputs: An output-stream type and a fixed test-owned codec label.
// Outputs: Requires all invalid levels to preserve existing bytes and leave missing destinations absent.
template <typename OutputStream> void require_non_destructive_stream_admission(const std::string& codec) {
    const auto root = test_temp_dir("stream-admission-" + codec);
    const std::string sentinel = "existing file must survive invalid compression options";
    for (const int level : {0, 10, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        for (const bool exists : {true, false}) {
            const auto path = root / (std::to_string(level) + (exists ? "-existing" : "-missing"));
            if (exists) {
                std::ofstream file(path, std::ios::binary);
                file.exceptions(std::ios::badbit | std::ios::failbit);
                file.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
                file.close();
            }
            bool rejected = false;
            try {
                OutputStream output(path, level);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            if (exists) {
                REQUIRE_EQ(read_stream_fixture(path), sentinel);
            } else {
                REQUIRE_TRUE(!std::filesystem::exists(path));
            }
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Create a mixed binary fixture with embedded NUL bytes, repeated ranges, and stream-buffer tails.
// Inputs: None; size stays below 257 KiB.
// Outputs: Returns immutable test input independent of filesystem or host load.
std::string make_stream_partition_fixture() {
    std::string input(256U * 1024U + 17U, '\0');
    std::uint32_t state = 0x17362549U;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] =
            index >= 16384U && index % 4096U < 2048U ? input[index % 16384U] : static_cast<char>(state >> 24U);
    }
    return input;
}

// Purpose: Write one valid codec stream, verify byte counts and close semantics, and reject writes after close.
// Inputs: Output type, test-owned path, immutable input, level 1-9, and whole versus fragmented bulk/single writes.
// Outputs: Requires accurate counters, safe caller-buffer reuse, idempotent close, and unchanged post-close bytes.
template <typename OutputStream>
void write_partitioned_stream(const std::filesystem::path& path, const std::string& input, int level, bool fragmented) {
    OutputStream output(path, level);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    const auto initial_output_bytes = output.output_bytes();
    output.write("", 0);
    REQUIRE_EQ(output.input_bytes(), 0U);
    REQUIRE_EQ(output.output_bytes(), initial_output_bytes);
    constexpr std::array<std::size_t, 6> partitions{1U, 3U, 8191U, 65535U, 65536U, 65537U};
    std::vector<char> scratch(partitions.back());
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < input.size()) {
        const auto count =
            std::min(input.size() - offset, fragmented ? partitions[partition++ % partitions.size()] : input.size());
        if (fragmented) {
            std::copy_n(input.data() + offset, count, scratch.data());
            if (count == 1U) {
                output.put(scratch.front());
            } else {
                output.write(scratch.data(), static_cast<std::streamsize>(count));
            }
            std::fill(scratch.begin(), scratch.end(), static_cast<char>(0xA5));
        } else {
            output.write(input.data() + offset, static_cast<std::streamsize>(count));
        }
        offset += count;
    }
    output.close();
    output.close();
    REQUIRE_EQ(output.input_bytes(), input.size());
    REQUIRE_EQ(output.output_bytes(), std::filesystem::file_size(path));
    const auto before = read_stream_fixture(path);
    bool rejected = false;
    try {
        output.put('x');
    } catch (const std::exception&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(output.input_bytes(), input.size());
    REQUIRE_EQ(read_stream_fixture(path), before);
}

// Purpose: Prove shared CPU-stream behavior across all supported efforts and caller write partitioning.
// Inputs: Matching output/input codec types and a fixed label for disposable files.
// Outputs: Requires byte-identical encodings, exact full decoding, and valid empty streams for levels 1-9.
template <typename OutputStream, typename InputStream>
void require_consistent_stream_partitioning(const std::string& codec) {
    const auto root = test_temp_dir("stream-partition-" + codec);
    const auto input = make_stream_partition_fixture();
    for (int level = 1; level <= 9; ++level) {
        const auto whole = root / (std::to_string(level) + "-whole");
        const auto fragmented = root / (std::to_string(level) + "-fragmented");
        write_partitioned_stream<OutputStream>(whole, input, level, false);
        write_partitioned_stream<OutputStream>(fragmented, input, level, true);
        REQUIRE_EQ(read_stream_fixture(whole), read_stream_fixture(fragmented));
        for (const auto& path : {whole, fragmented}) {
            InputStream decoded(path);
            const std::string restored{std::istreambuf_iterator<char>(decoded), std::istreambuf_iterator<char>()};
            REQUIRE_EQ(restored, input);
        }
        const auto empty = root / (std::to_string(level) + "-empty");
        write_partitioned_stream<OutputStream>(empty, {}, level, true);
        InputStream decoded(empty);
        REQUIRE_EQ(decoded.peek(), std::char_traits<char>::eof());
    }
    std::filesystem::remove_all(root);
}

// Purpose: Require explicit stream flushes to publish produced bytes and reject calls after close.
// Inputs: A shared output/input codec pair and one bounded binary fixture.
// Outputs: Checks on-disk byte count before close and full decoding after finalization.
template <typename OutputStream, typename InputStream> void require_stream_flush_contract(const std::string& codec) {
    const auto root = test_temp_dir("stream-flush-" + codec);
    const auto archive = root / "output";
    auto input = make_stream_partition_fixture();
    const auto block = input;
    for (int repeat = 0; repeat < 3; ++repeat) {
        input += block;
    }
    OutputStream output(archive, 5);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(input.data(), static_cast<std::streamsize>(input.size()));
    REQUIRE_TRUE(output.output_bytes() > 0U);
    output.flush();
    REQUIRE_EQ(std::filesystem::file_size(archive), output.output_bytes());
    output.close();
    bool rejected = false;
    try {
        output.flush();
    } catch (const std::ios_base::failure&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    {
        InputStream decoded(archive);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(decoded), std::istreambuf_iterator<char>()), input);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Require decoder errors to propagate through ordinary reads and remain terminal after flag resets.
// Inputs: Matching stream types, a fixed codec label, and the checksum's distance from the end of its frame.
// Outputs: Valid input still reaches ordinary EOF; damaged checksums and truncated trailers always throw.
template <typename OutputStream, typename InputStream>
void require_stream_read_failure(const std::string& codec, std::size_t checksum_offset) {
    const auto root = test_temp_dir("stream-read-failure-" + codec);
    const auto archive = root / "input";
    const std::string payload(257U, 'x');
    {
        OutputStream output(archive, 5);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.close();
    }
    const auto original = read_stream_fixture(archive);
    REQUIRE_TRUE(original.size() > checksum_offset);
    {
        InputStream input(archive);
        std::array<char, 4096> bytes{};
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE_TRUE(input.eof());
        REQUIRE_TRUE(!input.bad());
        REQUIRE_EQ(std::string(bytes.data(), static_cast<std::size_t>(input.gcount())), payload);
        input.finish();
        input.finish();
    }
    for (const bool truncate : {false, true}) {
        auto damaged = original;
        if (truncate) {
            damaged.pop_back();
        } else {
            damaged[damaged.size() - checksum_offset] ^= 1;
        }
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            output.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
            output.close();
        }
        {
            InputStream input(archive);
            std::array<char, 4096> bytes{};
            bool rejected = false;
            std::string original_error;
            try {
                input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            } catch (const flitzzip::ArchiveError& error) {
                rejected = true;
                original_error = error.what();
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(input.bad());
            for (int attempt = 0; attempt < 2; ++attempt) {
                input.clear();
                rejected = false;
                try {
                    input.finish();
                } catch (const flitzzip::ArchiveError& error) {
                    rejected = true;
                    REQUIRE_EQ(std::string(error.what()), original_error);
                }
                REQUIRE_TRUE(rejected);
            }
        }
        require_sticky_decoder_failure<InputStream>(archive);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent physical truncation of an admitted source and reject pre-existing shortened streams permanently.
// Inputs: Matching stream types and codec label; a bounded mixed corpus and only test-owned file mutations.
// Outputs: Real sharing denial with unchanged active bytes, terminal corruption after release, and exact valid reuse.
template <typename OutputStream, typename InputStream> void require_stream_source_truncation(const std::string& codec) {
    const auto root = test_temp_dir("stream-source-truncation-" + codec);
    const auto archive = root / "input";
    const auto payload = make_stream_partition_fixture();
    {
        OutputStream output(archive, 5);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.close();
    }
    const auto original = read_stream_fixture(archive);
    REQUIRE_TRUE(original.size() > 65536U);
    {
        InputStream input(archive);
        REQUIRE_EQ(input.input_bytes(), original.size());
        std::error_code mutation_error;
        std::filesystem::resize_file(archive, 0, mutation_error);
        REQUIRE_EQ(mutation_error.value(), static_cast<int>(ERROR_SHARING_VIOLATION));
        REQUIRE_EQ(read_stream_fixture(archive), original);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), payload);
        input.finish();
    }
    std::filesystem::resize_file(archive, original.size() / 2U);
    require_sticky_decoder_failure<InputStream>(archive);
    {
        std::ofstream restored(archive, std::ios::binary | std::ios::trunc);
        restored.exceptions(std::ios::badbit | std::ios::failbit);
        restored.write(original.data(), static_cast<std::streamsize>(original.size()));
        restored.close();
    }
    {
        InputStream input(archive);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), payload);
        input.finish();
    }
    std::filesystem::remove_all(root);
}

}  // namespace

// Purpose: Prevent dynamic Huffman metadata from overwhelming a short repeated sequence.
// Inputs: A 511-byte period-three fixture at all efforts with whole and one-byte output windows.
// Outputs: Requires default coding no larger than forced fixed coding and byte-identical incremental output.
TEST_CASE(compression_stream_miniz_selects_smaller_huffman_block) {
    std::string input(511U, '\0');
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<char>('a' + i % 3U);
    }
    for (int level = 1; level <= 9; ++level) {
        const auto fixed = encode_miniz_candidate(input, level, MZ_FIXED, 65536U);
        const auto selected = encode_miniz_candidate(input, level, MZ_DEFAULT_STRATEGY, 65536U);
        REQUIRE_TRUE(selected.size() <= fixed.size());
        REQUIRE_TRUE(selected.size() > 2U);
        REQUIRE_EQ((selected[2] >> 1U) & 3U, 1U);
        REQUIRE_EQ(encode_miniz_candidate(input, level, MZ_DEFAULT_STRATEGY, 1U), selected);
        require_miniz_candidate_roundtrip(selected, input);
    }
}

// Purpose: Check block-type cost selection over mixed alphabets, multiple blocks, and output-buffer boundaries.
// Inputs: Four deterministic distributions across six sizes and three search efforts.
// Outputs: Requires default coding no larger than forced fixed coding and exact decoding under fragmented output.
TEST_CASE(compression_stream_miniz_huffman_cost_corpus) {
    bool observed_dynamic_saving = false;
    for (const auto size : {512U, 4096U, 32768U, 65536U, 131073U, 262145U}) {
        for (unsigned int profile = 0; profile < 4U; ++profile) {
            std::string input(size, '\0');
            std::uint32_t state = 0x91AF386BU;
            for (std::size_t i = 0; i < input.size(); ++i) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                const auto value = static_cast<unsigned char>(state >> 24U);
                input[i] = static_cast<char>(profile == 1U ? value & 15U : value);
                if (profile == 2U && i >= 16384U) {
                    input[i] = input[i % 16384U];
                } else if (profile == 3U && i % 32768U < 16384U) {
                    input[i] = static_cast<char>(i % 3U);
                }
            }
            for (const int level : {1, 5, 9}) {
                const auto fixed = encode_miniz_candidate(input, level, MZ_FIXED, 65536U);
                const auto selected = encode_miniz_candidate(input, level, MZ_DEFAULT_STRATEGY, 65536U);
                REQUIRE_TRUE(selected.size() <= fixed.size());
                observed_dynamic_saving = observed_dynamic_saving || selected.size() < fixed.size();
                REQUIRE_EQ(encode_miniz_candidate(input, level, MZ_DEFAULT_STRATEGY, 257U), selected);
                require_miniz_candidate_roundtrip(selected, input);
            }
        }
    }
    REQUIRE_TRUE(observed_dynamic_saving);
}

// Purpose: Protect Gzip destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_gzip_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<flitzzip::GzipOutputStream>("gzip");
}

// Purpose: Protect Bzip2 destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_bzip2_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<flitzzip::Bzip2OutputStream>("bzip2");
}

// Purpose: Protect Zstandard destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_zstd_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<flitzzip::ZstdOutputStream>("zstd");
}

// Purpose: Prove worker-scoped Zstandard context reuse leaves independent native frame bytes unchanged.
// Inputs: Three deterministic binary shapes and all product effort levels, encoded repeatedly through one context.
// Outputs: Requires exact one-shot frame parity and independent byte-exact decoding for every case.
TEST_CASE(compression_stream_zstd_reused_context_matches_one_shot) {
    const auto& zstd = flitzzip::zstd_runtime();
    const auto release_context = [&zstd](flitzzip::ZstdCompressionContext* context) {
        zstd.free_compression_context(context);
    };
    std::unique_ptr<flitzzip::ZstdCompressionContext, decltype(release_context)> context(
        zstd.create_compression_context(), release_context);
    REQUIRE_TRUE(context != nullptr);

    std::array<std::vector<std::byte>, 3> inputs;
    for (auto& input : inputs) {
        input.resize(65536U + 17U);
    }
    std::uint32_t random_state = 0xA31B2C49U;
    for (std::size_t index = 0U; index < inputs.front().size(); ++index) {
        random_state ^= random_state << 13U;
        random_state ^= random_state >> 17U;
        random_state ^= random_state << 5U;
        inputs[0][index] = static_cast<std::byte>(random_state & 0xFFU);
        inputs[1][index] = static_cast<std::byte>(index % 251U);
        inputs[2][index] = static_cast<std::byte>((index / 1024U) % 7U == 0U ? random_state & 0xFFU : index % 97U);
    }
    for (int level = 1; level <= 9; ++level) {
        for (const auto& input : inputs) {
            const auto capacity = zstd.block_compress_bound(input.size());
            REQUIRE_TRUE(!zstd.is_error(capacity));
            std::vector<std::byte> one_shot(capacity);
            std::vector<std::byte> reused(capacity);
            const auto one_shot_bytes =
                zstd.compress_block(one_shot.data(), one_shot.size(), input.data(), input.size(), level);
            const auto reused_bytes = zstd.compress_block_with_context(context.get(), reused.data(), reused.size(),
                                                                       input.data(), input.size(), level);
            REQUIRE_TRUE(!zstd.is_error(one_shot_bytes));
            REQUIRE_EQ(reused_bytes, one_shot_bytes);
            REQUIRE_TRUE(std::equal(one_shot.begin(), one_shot.begin() + static_cast<std::ptrdiff_t>(one_shot_bytes),
                                    reused.begin()));
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(zstd.decompress_block(decoded.data(), decoded.size(), reused.data(), reused_bytes),
                       input.size());
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Check bounded single-pass decoding and independence after failed jobs through the actual pinned DLL.
// Inputs: Empty, short, and multi-block binary frames at all nine efforts, damaged framing, and short destinations.
// Outputs: Requires runtime errors for invalid jobs followed by an independent byte-exact valid decode.
TEST_CASE(compression_stream_zstd_block_decoder_bounds_and_roundtrip) {
    const auto& zstd = flitzzip::zstd_runtime();
    for (int level = 1; level <= 9; ++level) {
        for (const auto size : {0U, 1U, 127U, 65553U, 1048593U, 4096U}) {
            std::vector<std::byte> input(size);
            std::uint32_t state = 0xB13F29A7U;
            for (std::size_t i = 0; i < input.size(); ++i) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                input[i] = static_cast<std::byte>((i / 4096U) % 3U == 0U ? state & 255U : i % 97U);
            }
            const auto capacity = zstd.block_compress_bound(input.size());
            REQUIRE_TRUE(!zstd.is_error(capacity));
            std::vector<std::byte> encoded(capacity);
            const auto written = zstd.compress_block(encoded.data(), encoded.size(), input.data(), input.size(), level);
            REQUIRE_TRUE(!zstd.is_error(written) && written > 0U && written <= encoded.size());
            encoded.resize(written);
            auto damaged = encoded;
            damaged.front() ^= std::byte{0xFF};
            std::vector<std::byte> actual(input.size());
            REQUIRE_TRUE(
                zstd.is_error(zstd.decompress_block(actual.data(), actual.size(), damaged.data(), damaged.size())));
            if (!input.empty()) {
                REQUIRE_TRUE(zstd.is_error(
                    zstd.decompress_block(actual.data(), actual.size() - 1U, encoded.data(), encoded.size())));
            }
            REQUIRE_EQ(zstd.decompress_block(actual.data(), actual.size(), encoded.data(), encoded.size()),
                       input.size());
            REQUIRE_EQ(actual, input);
        }
    }
}

// Purpose: Verify Gzip stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_gzip_partition_and_effort_parity) {
    require_consistent_stream_partitioning<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("gzip");
}

// Purpose: Verify Bzip2 stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_bzip2_partition_and_effort_parity) {
    require_consistent_stream_partitioning<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>("bzip2");
}

// Purpose: Verify Zstandard stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_zstd_partition_and_effort_parity) {
    require_consistent_stream_partitioning<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("zstd");
}

// Purpose: Preserve the same observable output-flush contract across all shared CPU codecs.
// Inputs: Bounded mixed binary payload at product effort five.
// Outputs: Requires published partial bytes, terminal post-close flush errors, and exact restored content.
TEST_CASE(compression_stream_flush_contract) {
    require_stream_flush_contract<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("gzip");
    require_stream_flush_contract<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>("bzip2");
    require_stream_flush_contract<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("zstd");
}

// Purpose: Keep malformed Gzip reads distinct from successful end of input.
// Inputs: A valid tiny member, then a damaged CRC and a truncated trailer.
// Outputs: Requires immediate decoder errors and repeated rejection after stream-state resets.
TEST_CASE(compression_stream_gzip_read_failure_is_terminal) {
    require_stream_read_failure<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("gzip", 8U);
}

// Purpose: Keep malformed Zstandard reads distinct from successful end of input.
// Inputs: A valid tiny frame, then a damaged checksum and a truncated trailer.
// Outputs: Requires immediate decoder errors and repeated rejection after stream-state resets.
TEST_CASE(compression_stream_zstd_read_failure_is_terminal) {
    require_stream_read_failure<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("zstd", 4U);
}

// Purpose: Refuse truncation during decoding and retain terminal corruption rejection after the source is released.
// Inputs: Three bounded codec files, real Windows resize attempts and independently shortened closed-source frames.
// Outputs: Active bytes remain exact; malformed frames reject permanently and subsequent valid streams decode.
TEST_CASE(compression_stream_source_truncation_is_terminal) {
    require_stream_source_truncation<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("Gzip");
    require_stream_source_truncation<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>("Bzip2");
    require_stream_source_truncation<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("Zstandard");
}

// Purpose: Establish byte-for-byte equivalence between standalone Gzip and the stream used by container wrappers.
// Inputs: Empty and mixed binary files at every supported effort; outputs are bounded temporary fixtures.
// Outputs: Requires complete encoded-byte identity and matching standalone telemetry before and after consolidation.
TEST_CASE(compression_stream_gzip_adapter_wire_parity) {
    const auto root = test_temp_dir("gzip-adapter-stream-parity");
    for (const auto& input : {std::string{}, make_stream_partition_fixture()}) {
        const auto source = root / "source.bin";
        {
            std::ofstream file(source, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(input.data(), static_cast<std::streamsize>(input.size()));
            file.close();
        }
        for (int level = 1; level <= 9; ++level) {
            const auto archive = root / "standalone.gz";
            const auto stream = root / "stream.gz";
            const auto stats = flitzzip::compress_gzip_file(source, archive, level);
            write_partitioned_stream<flitzzip::GzipOutputStream>(stream, input, level, true);
            REQUIRE_EQ(read_stream_fixture(archive), read_stream_fixture(stream));
            REQUIRE_EQ(stats.input_bytes, input.size());
            REQUIRE_EQ(stats.output_bytes, std::filesystem::file_size(archive));
            REQUIRE_TRUE(!stats.gpu_used);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve atomic publication and temporary cleanup when a Gzip progress callback interrupts compression.
// Inputs: A bounded source, existing destination sentinel, and callback that throws after the first processed bytes.
// Outputs: Requires the exception to propagate, the destination to stay unchanged, and temporary state to be removed.
TEST_CASE(compression_stream_gzip_callback_failure_preserves_output) {
    const auto root = test_temp_dir("gzip-callback-failure");
    const auto source = root / "source.bin";
    const auto archive = root / "destination.gz";
    const auto input = make_stream_partition_fixture();
    const std::string sentinel = "existing destination";
    for (const auto& [path, bytes] : {std::pair{source, input}, std::pair{archive, sentinel}}) {
        std::ofstream file(path, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
    }
    bool interrupted = false;
    try {
        (void)flitzzip::compress_gzip_file(source, archive, 5, [](const flitzzip::ProgressSnapshot& progress) {
            if (progress.processed_bytes > 0U) {
                throw std::runtime_error("test progress interruption");
            }
        });
    } catch (const std::runtime_error& error) {
        interrupted = std::string(error.what()) == "test progress interruption";
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_EQ(read_stream_fixture(archive), sentinel);
    REQUIRE_EQ(read_stream_fixture(source), input);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator()), 2);
    std::filesystem::remove_all(root);
}

namespace {

// Purpose: Exercise exact-size streaming with immediate caller-buffer reuse and explicit workspace accounting.
// Inputs: A disposable path, immutable bounded fixture, effort 1-9, and whole/fragmented write selection.
// Outputs: Returns complete encoded bytes and observed codec workspace; requires exact counters and released state.
std::pair<std::string, std::size_t> write_sized_zstd_stream(const std::filesystem::path& path, const std::string& input,
                                                            int level, bool fragmented) {
    flitzzip::ZstdOutputStream output(path, level, input.size());
    output.exceptions(std::ios::badbit | std::ios::failbit);
    std::size_t peak = output.workspace_bytes();
    for (std::size_t offset = 0; offset < input.size();) {
        const auto count = std::min(input.size() - offset, fragmented ? 8191U : input.size());
        auto scratch = input.substr(offset, count);
        output.write(scratch.data(), static_cast<std::streamsize>(count));
        std::fill(scratch.begin(), scratch.end(), static_cast<char>(0xA5));
        peak = std::max(peak, output.workspace_bytes());
        offset += count;
    }
    output.close();
    output.close();
    REQUIRE_EQ(output.input_bytes(), input.size());
    REQUIRE_EQ(output.output_bytes(), std::filesystem::file_size(path));
    REQUIRE_EQ(output.workspace_bytes(), 0U);
    return {read_stream_fixture(path), peak};
}

}  // namespace

// Purpose: Keep exact-size admission compatible with every product effort and caller write partition.
// Inputs: Empty/tiny/boundary fixtures, levels 1-9, and matching standalone/shared writers.
// Outputs: Requires exact decoding, matching wire bytes, bounded small-file workspace, and accurate adapter telemetry.
TEST_CASE(compression_stream_zstd_declared_size_contract) {
    const auto root = test_temp_dir("zstd-declared-size");
    const auto fixture = make_stream_partition_fixture();
    for (const auto length : {0U, 1U, 257U, 65536U, 262161U}) {
        const auto input = fixture.substr(0, length);
        const auto source = root / "source.bin";
        {
            std::ofstream file(source, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(input.data(), static_cast<std::streamsize>(input.size()));
        }
        for (int level = 1; level <= 9; ++level) {
            const auto whole = root / "whole.zst";
            const auto fragmented = root / "fragmented.zst";
            const auto archive = root / "standalone.zst";
            const auto [encoded, workspace] = write_sized_zstd_stream(whole, input, level, false);
            const auto [partitioned, partition_workspace] = write_sized_zstd_stream(fragmented, input, level, true);
            REQUIRE_EQ(encoded, partitioned);
            REQUIRE_TRUE(workspace < 16U * 1024U * 1024U);
            REQUIRE_TRUE(partition_workspace < 16U * 1024U * 1024U);
            const auto stats = flitzzip::compress_zstd({source}, archive, level);
            REQUIRE_EQ(read_stream_fixture(archive), encoded);
            REQUIRE_EQ(stats.input_bytes, input.size());
            REQUIRE_EQ(stats.output_bytes, encoded.size());
            flitzzip::ZstdInputStream decoded(archive);
            const std::string restored{std::istreambuf_iterator<char>(decoded), std::istreambuf_iterator<char>()};
            decoded.finish();
            REQUIRE_EQ(restored, input);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject exact-size overruns and underruns instead of completing a frame with an incorrect promise.
// Inputs: Zero/nonzero pledges, split oversized writes, short finalization, and the reserved unknown sentinel.
// Outputs: Requires errors before excess bytes are counted and requires overrun state to prevent close success.
TEST_CASE(compression_stream_zstd_declared_size_mismatch) {
    const auto root = test_temp_dir("zstd-size-mismatch");
    for (const std::uint64_t promised : {0U, 1U, 4U}) {
        flitzzip::ZstdOutputStream output(root / "overrun.zst", 5, promised);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        const std::string valid(static_cast<std::size_t>(promised), 'A');
        output.write(valid.data(), static_cast<std::streamsize>(valid.size()));
        bool rejected = false;
        try {
            output.put('x');
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(output.input_bytes(), promised);
        rejected = false;
        try {
            output.close();
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(output.workspace_bytes(), 0U);
    }
    for (const bool write_prefix : {false, true}) {
        flitzzip::ZstdOutputStream output(root / "underrun.zst", 5, 4U);
        if (write_prefix) {
            output.write("abc", 3);
        }
        bool rejected = false;
        try {
            output.close();
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(output.workspace_bytes(), 0U);
    }
    const auto sentinel = root / "sentinel.zst";
    {
        std::ofstream file(sentinel, std::ios::binary);
        file << "unchanged";
    }
    bool rejected = false;
    try {
        flitzzip::ZstdOutputStream output(sentinel, 5, std::numeric_limits<std::uint64_t>::max());
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_stream_fixture(sentinel), "unchanged");
    std::filesystem::remove_all(root);
}

// Purpose: Enforce stream admission bounds across small, unknown, and limited-core host configurations.
// Inputs: Threshold byte counts, every effort, and representative logical processor availability.
// Outputs: Requires synchronous small/high-effort operation and bounded workers with caller headroom.
TEST_CASE(compression_stream_zstd_worker_admission) {
    constexpr std::uint64_t threshold = 16U * 1024U * 1024U;
    for (int level = 1; level <= 9; ++level) {
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(std::nullopt, level, 64U), 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(threshold - 1U, level, 64U), 0U);
        for (unsigned int processors : {0U, 1U, 2U, 3U}) {
            REQUIRE_EQ(flitzzip::zstd_stream_worker_count(threshold, level, processors), 0U);
        }
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(threshold, level, 4U), level < 7 ? 1U : 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(threshold, level, 64U), level < 7 ? 2U : 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(2U * threshold - 1U, level, 64U), level < 7 ? 3U : 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(2U * threshold, level, 64U), level < 7 ? 4U : 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(8U * threshold, level, 64U), level < 7 ? 4U : 0U);
        REQUIRE_EQ(flitzzip::zstd_stream_worker_count(std::numeric_limits<std::uint64_t>::max(), level, 64U),
                   level < 7 ? 4U : 0U);
    }
}

namespace {

// Present one owned binary window lazily so tests can observe consumption and
// prove failed sinks/zero requests do not advance or decode the source.
class DecodedWindowFixture final : public flitzzip::DecodedInputBuffer {
  public:
    explicit DecodedWindowFixture(std::string bytes) : bytes_(std::move(bytes)) {}

    const char* window_begin() const {
        return bytes_.data();
    }
    std::size_t consumed() const {
        return published_ ? static_cast<std::size_t>(gptr() - eback()) : 0U;
    }
    unsigned int underflow_calls = 0U;
    bool source_failure = false;

  protected:
    int_type underflow() override {
        ++underflow_calls;
        if (source_failure) {
            throw flitzzip::ArchiveError("independent source failure");
        }
        if (published_ || bytes_.empty()) {
            return traits_type::eof();
        }
        published_ = true;
        setg(bytes_.data(), bytes_.data(), bytes_.data() + bytes_.size());
        return traits_type::to_int_type(*gptr());
    }

  private:
    std::string bytes_;
    bool published_ = false;
};

// Borrow already constructed storage and expose authoritative finish validation.
// No file, decoder allocation or global fault hook is involved.
class FinishProbeInput final : public std::istream {
  public:
    explicit FinishProbeInput(std::streambuf* source) : std::istream(source) {}
    unsigned int finish_calls = 0U;
    bool fail_finish = false;

    void finish() {
        ++finish_calls;
        if (fail_finish) {
            throw flitzzip::ArchiveError("independent finish failure");
        }
    }
};

// Retain byte values and non-owning address identities only. Partial writes and
// original sink exceptions challenge the production transfer boundary safely.
class DecodedSinkFixture final : public std::streambuf {
  public:
    std::string written;
    std::vector<const void*> addresses;
    bool short_write = false;
    bool throw_on_write = false;

  protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0) {
            return 0;
        }
        addresses.push_back(data);
        if (throw_on_write) {
            throw std::runtime_error("independent sink failure");
        }
        const auto accepted = short_write ? count - 1 : count;
        written.append(data, static_cast<std::size_t>(accepted));
        return accepted;
    }
};

// A provider returning a character without an owned window violates the
// transfer contract and must not masquerade as either data or successful EOF.
class UnpublishedDecodedWindow final : public flitzzip::DecodedInputBuffer {
  protected:
    int_type underflow() override {
        return traits_type::to_int_type('x');
    }
};

// Exercise the actual codec reader and standalone extractor with bounded binary
// fixtures. Mixed standard reads, direct chunks, putback, finish and counters must
// agree with independent source bytes; no timing or throughput is collected.
template <class Writer, class Reader, class Extractor>
void require_decoded_chunk_transfer(const char* extension, Extractor extract) {
    flitzzip_test::ScopedTestDirectory cleanup{test_temp_dir(std::string("decoded-transfer-") + extension)};
    const auto& root = cleanup.path;
    const auto path = root / (std::string("payload.") + extension);
    std::string expected(3U * 129U * 1024U + 17U, '\0');
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        expected[index] = static_cast<char>((index * 31U + index / 97U) & 255U);
    }
    {
        Writer output(path, 5);
        output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        output.close();
    }
    {
        Reader input(path);
        std::ostringstream output;
        REQUIRE_EQ(input.transfer_to(output, 0U), 0U);
        REQUIRE_EQ(input.output_bytes(), 0U);
        std::array<char, 13> prefix{};
        input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(prefix.size()));
        output.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        constexpr std::array<std::size_t, 7> limits{
            1U, 17U, 63U, 65536U, 132096U, 4096U, std::numeric_limits<std::size_t>::max()};
        std::size_t call = 0U;
        for (;;) {
            const auto limit = limits[call++ % limits.size()];
            const auto old_count = input.gcount();
            const auto bytes = input.transfer_to(output, limit);
            REQUIRE_TRUE(bytes <= limit);
            REQUIRE_EQ(input.gcount(), old_count);
            if (bytes == 0U) {
                break;
            }
            if (input.peek() != std::char_traits<char>::eof()) {
                char next = 0;
                input.get(next);
                input.unget();
                REQUIRE_TRUE(input.good());
            }
        }
        input.finish();
        REQUIRE_EQ(output.str(), expected);
        REQUIRE_EQ(input.output_bytes(), expected.size());
        REQUIRE_EQ(input.input_bytes(), std::filesystem::file_size(path));
    }
    {
        Reader input(path);
        std::ostringstream prefix;
        REQUIRE_EQ(input.transfer_to(prefix, 7U), 7U);
        input.finish();
        input.finish();
        REQUIRE_EQ(prefix.str(), expected.substr(0U, 7U));
        REQUIRE_EQ(input.output_bytes(), expected.size());
    }
    const auto destination = root / "restored";
    const auto stats = extract(path, destination, false, {});
    REQUIRE_EQ(stats.output_bytes, expected.size());
    REQUIRE_EQ(stats.input_bytes, std::filesystem::file_size(path));
    REQUIRE_TRUE(!stats.gpu_used);
    REQUIRE_EQ(read_stream_fixture(destination / "payload"), expected);
    const auto empty_path = root / (std::string("empty.") + extension);
    {
        Writer writer(empty_path, 5);
        writer.close();
    }
    {
        Reader input(empty_path);
        std::ostringstream output;
        REQUIRE_EQ(input.transfer_to(output, 65536U), 0U);
        input.finish();
        REQUIRE_TRUE(output.str().empty());
        REQUIRE_EQ(input.output_bytes(), 0U);
    }
    REQUIRE_EQ(extract(empty_path, destination, false, {}).output_bytes, 0U);
    REQUIRE_TRUE(std::filesystem::exists(destination / "empty"));
    REQUIRE_EQ(std::filesystem::file_size(destination / "empty"), 0U);
    std::filesystem::remove_all(root);
}

}  // namespace

// The sink must see the decoder's owned window, not a transient staging copy.
// The old read-then-write path is an independent control for pointer sensitivity.
TEST_CASE(decoded_input_buffer_transfers_owned_window_without_staging) {
    const std::string expected("a\0b\xFF"
                               "cde",
                               7U);
    DecodedWindowFixture direct(expected);
    DecodedSinkFixture direct_sink;
    std::ostream direct_output(&direct_sink);
    REQUIRE_EQ(direct.transfer_to(direct_output, 4U), 4U);
    REQUIRE_EQ(direct_sink.addresses.front(), direct.window_begin());
    DecodedWindowFixture staged(expected);
    DecodedSinkFixture staged_sink;
    std::istream input(&staged);
    std::ostream staged_output(&staged_sink);
    std::array<char, 4> scratch{};
    input.read(scratch.data(), static_cast<std::streamsize>(scratch.size()));
    staged_output.write(scratch.data(), static_cast<std::streamsize>(scratch.size()));
    REQUIRE_TRUE(staged_sink.addresses.front() != staged.window_begin());
    REQUIRE_EQ(staged_sink.written, direct_sink.written);
    REQUIRE_EQ(direct.transfer_to(direct_output, std::numeric_limits<std::size_t>::max()), 3U);
    REQUIRE_EQ(direct_sink.addresses.back(), direct.window_begin() + 4U);
    REQUIRE_EQ(direct.transfer_to(direct_output, 1U), 0U);
    REQUIRE_EQ(direct_sink.written, expected);
}

// The visitor must see actual decoder storage at each bounded window. Complete
// bytes and an independently derived byte sum also guard payload/checksum use.
TEST_CASE(stream_byte_consumer_borrows_decoded_storage_and_preserves_tail) {
    std::string expected(2U * 65536U + 17U, '\0');
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<char>(index & 255U);
    }
    DecodedWindowFixture source(expected + "TRAILER");
    std::istream input(&source);
    std::vector<const char*> addresses;
    std::string observed;
    std::uint32_t sum = 0;
    flitzzip::consume_exact_stream_bytes(
        input, expected.size(),
        [&](std::span<const char> bytes) {
            REQUIRE_TRUE(bytes.size() <= 65536U);
            addresses.push_back(bytes.data());
            observed.append(bytes.data(), bytes.size());
            for (const unsigned char value : bytes) {
                sum += value;
            }
        },
        "fixture payload");
    REQUIRE_EQ(observed, expected);
    REQUIRE_EQ(sum, 16711816U);
    REQUIRE_EQ(addresses.size(), 3U);
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        REQUIRE_EQ(addresses[index], source.window_begin() + index * 65536U);
    }
    REQUIRE_EQ(source.consumed(), expected.size());
    std::array<char, 7> tail{};
    input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    REQUIRE_EQ(std::string(tail.data(), tail.size()), "TRAILER");
}

TEST_CASE(stream_byte_consumer_preserves_borrowed_window_after_visitor_failure) {
    DecodedWindowFixture source("abcdefg");
    std::istream input(&source);
    input.setstate(std::ios::badbit);
    unsigned int visits = 0U;
    const auto visit = [&](std::span<const char>) { ++visits; };
    flitzzip::consume_exact_stream_bytes(input, 0U, visit, "fixture");
    bool failed = false;
    try {
        flitzzip::consume_exact_stream_bytes(input, 1U, visit, "fixture");
    } catch (const flitzzip::ArchiveError&) {
        failed = true;
    }
    REQUIRE_TRUE(failed && visits == 0U && source.underflow_calls == 0U);
    input.clear();
    failed = false;
    try {
        flitzzip::consume_exact_stream_bytes(
            input, 4U, [](std::span<const char>) { throw std::runtime_error("independent visitor failure"); },
            "fixture");
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "independent visitor failure") {
            throw;
        }
        failed = true;
    }
    REQUIRE_TRUE(failed);
    REQUIRE_EQ(source.consumed(), 0U);
    std::string recovered;
    flitzzip::consume_exact_stream_bytes(
        input, 7U, [&](std::span<const char> bytes) { recovered.append(bytes.data(), bytes.size()); }, "fixture");
    REQUIRE_EQ(recovered, "abcdefg");
}

TEST_CASE(stream_byte_consumer_plain_reads_and_truncation_are_exact) {
    std::istringstream input("abcdefgTRAILER");
    std::string observed;
    const auto consume = [&](std::span<const char> bytes) { observed.append(bytes.data(), bytes.size()); };
    flitzzip::consume_exact_stream_bytes(input, 7U, consume, "fixture");
    REQUIRE_EQ(observed, "abcdefg");
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), "TRAILER");
    std::istringstream short_input("abc");
    observed.clear();
    bool failed = false;
    try {
        flitzzip::consume_exact_stream_bytes(short_input, 4U, consume, "short fixture");
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "short fixture is truncated");
        failed = true;
    }
    REQUIRE_TRUE(failed && observed.empty());
    DecodedWindowFixture short_decoded("abc");
    std::istream decoded(&short_decoded);
    failed = false;
    try {
        flitzzip::consume_exact_stream_bytes(decoded, 4U, consume, "short fixture");
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "short fixture is truncated");
        failed = true;
    }
    REQUIRE_TRUE(failed);
}

// Checkpoint closures own their observer and status; unknown validation totals
// never masquerade as completed work. Exceptions retain their original type.
TEST_CASE(archive_phase_checkpoints_own_status_and_preserve_exceptions) {
    unsigned int calls = 0U;
    flitzzip::ProgressSnapshot observed;
    flitzzip::ProgressCheckpoint owned;
    {
        flitzzip::ProgressCallback callback = [&](const flitzzip::ProgressSnapshot& snapshot) {
            ++calls;
            observed = snapshot;
        };
        flitzzip::ProgressSnapshot snapshot{.operation = flitzzip::OperationKind::Extract, .note = "owned phase"};
        owned = flitzzip::make_archive_phase_checkpoint(callback, snapshot);
        snapshot.note = "changed after copy";
        callback = {};
    }
    flitzzip::invoke_archive_checkpoint(owned);
    REQUIRE_EQ(calls, 1U);
    REQUIRE_EQ(observed.note, "owned phase");
    const flitzzip::ProgressCallback observe = [&](const flitzzip::ProgressSnapshot& snapshot) { observed = snapshot; };
    flitzzip::invoke_archive_checkpoint(flitzzip::make_archive_validation_checkpoint(observe));
    REQUIRE_TRUE(observed.operation == flitzzip::OperationKind::Extract);
    REQUIRE_EQ(observed.total_bytes, 0U);
    REQUIRE_EQ(observed.processed_bytes, 0U);
    REQUIRE_EQ(observed.total_entries, 0U);
    REQUIRE_EQ(observed.completed_entries, 0U);
    flitzzip::invoke_archive_checkpoint(flitzzip::make_archive_finishing_checkpoint(observe, 71U, 3U));
    REQUIRE_EQ(observed.total_bytes, 71U);
    REQUIRE_EQ(observed.processed_bytes, 71U);
    REQUIRE_EQ(observed.total_entries, 3U);
    REQUIRE_EQ(observed.completed_entries, 3U);
    REQUIRE_TRUE(!flitzzip::make_archive_validation_checkpoint({}));
    REQUIRE_TRUE(!flitzzip::make_archive_finishing_checkpoint({}, 71U, 3U));
    flitzzip::invoke_archive_checkpoint({});
    bool aborted = false;
    try {
        flitzzip::invoke_archive_checkpoint(flitzzip::make_archive_validation_checkpoint(
            [](const flitzzip::ProgressSnapshot&) { throw ContainerProgressAbort{}; }));
    } catch (const ContainerProgressAbort&) {
        aborted = true;
    }
    REQUIRE_TRUE(aborted);
}

// Final draining is cancellable between bounded windows and still calls the
// original finish validator after EOF. A successful drain cannot conceal it.
TEST_CASE(archive_stream_finish_checkpoints_preserve_validation_and_recovery) {
    const std::string bytes(2U * 65536U + 17U, 'F');
    DecodedWindowFixture source(bytes);
    FinishProbeInput input(&source);
    unsigned int calls = 0U;
    bool aborted = false;
    try {
        flitzzip::finish_archive_stream(input, [&] {
            if (++calls == 2U) {
                throw ContainerProgressAbort{};
            }
        });
    } catch (const ContainerProgressAbort&) {
        aborted = true;
    }
    REQUIRE_TRUE(aborted);
    REQUIRE_EQ(source.consumed(), 65536U);
    REQUIRE_EQ(input.finish_calls, 0U);
    source.source_failure = true;
    bool failed = false;
    try {
        flitzzip::finish_archive_stream(input, [] {});
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "independent source failure");
        failed = true;
    }
    REQUIRE_TRUE(failed);
    REQUIRE_EQ(input.finish_calls, 0U);
    source.source_failure = false;
    input.fail_finish = true;
    failed = false;
    try {
        flitzzip::finish_archive_stream(input, [] {});
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "independent finish failure");
        failed = true;
    }
    REQUIRE_TRUE(failed);
    REQUIRE_EQ(source.consumed(), bytes.size());
    REQUIRE_EQ(input.finish_calls, 1U);
    input.fail_finish = false;
    flitzzip::finish_archive_stream(input, [] {});
    REQUIRE_EQ(input.finish_calls, 2U);
}

TEST_CASE(archive_stream_finish_rejects_unobservable_reader_without_hidden_drain) {
    std::stringbuf source("untouched");
    FinishProbeInput input(&source);
    unsigned int checks = 0U;
    bool rejected = false;
    try {
        flitzzip::finish_archive_stream(input, [&] { ++checks; });
    } catch (const std::logic_error& error) {
        REQUIRE_EQ(std::string(error.what()), "checkpointed archive decoder has no owned window interface");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(checks, 0U);
    REQUIRE_EQ(input.finish_calls, 0U);
    REQUIRE_EQ(input.peek(), 'u');
    flitzzip::finish_archive_stream(input, {});
    REQUIRE_EQ(input.finish_calls, 1U);
    REQUIRE_EQ(input.peek(), 'u');
}

// Ordinary and borrowed-window sources must stop before reading the next window.
// Independent suffix bytes distinguish a real checkpoint from post-read reporting.
TEST_CASE(stream_byte_consumer_checkpoints_preserve_next_unread_window) {
    std::string bytes(2U * 65536U + 17U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((index * 17U + index / 71U) & 255U);
    }
    for (const bool borrowed : {false, true}) {
        DecodedWindowFixture decoded(bytes);
        std::stringbuf ordinary(bytes);
        std::istream input(borrowed ? static_cast<std::streambuf*>(&decoded) : &ordinary);
        std::string observed;
        const auto consume = [&](std::span<const char> window) { observed.append(window.data(), window.size()); };
        unsigned int checks = 0U;
        bool aborted = false;
        try {
            flitzzip::consume_exact_stream_bytes(input, bytes.size(), consume, "checkpoint fixture", [&] {
                if (++checks == 2U) {
                    throw ContainerProgressAbort{};
                }
            });
        } catch (const ContainerProgressAbort&) {
            aborted = true;
        }
        REQUIRE_TRUE(aborted);
        REQUIRE_EQ(observed, bytes.substr(0U, 65536U));
        const std::string tail{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        REQUIRE_EQ(tail, bytes.substr(65536U));
    }
    DecodedWindowFixture final_source("end");
    std::istream final_input(&final_source);
    unsigned int checks = 0U;
    bool aborted = false;
    try {
        flitzzip::consume_exact_stream_bytes(
            final_input, 3U, [](std::span<const char>) {}, "final checkpoint",
            [&] {
                if (++checks == 2U) {
                    throw ContainerProgressAbort{};
                }
            });
    } catch (const ContainerProgressAbort&) {
        aborted = true;
    }
    REQUIRE_TRUE(aborted);
    REQUIRE_EQ(final_source.consumed(), 3U);
    flitzzip::consume_exact_stream_bytes(
        final_input, 0U, [](std::span<const char>) {}, "zero checkpoint", [] { throw ContainerProgressAbort{}; });
}

// Final checkpoints observe all requested bytes but never the following suffix.
// Literal boundary sequences independently cover ordinary and decoder-owned windows;
// exceptions at that final checkpoint preserve the suffix and original exception type.
TEST_CASE(stream_byte_consumer_final_checkpoints_preserve_tail_and_exception) {
    struct Boundary {
        std::size_t bytes;
        std::vector<std::size_t> checkpoints;
    };
    const std::array<Boundary, 5> boundaries{{{0U, {}},
                                              {1U, {0U, 1U}},
                                              {65536U, {0U, 65536U}},
                                              {65537U, {0U, 65536U, 65537U}},
                                              {131089U, {0U, 65536U, 131072U, 131089U}}}};
    for (const auto& boundary : boundaries) {
        const auto expected = flitzzip_test::stream_cancellation_payload().substr(0U, boundary.bytes);
        for (const bool borrowed : {false, true}) {
            for (const bool fail_final : {false, true}) {
                DecodedWindowFixture decoded(expected + "unread tail");
                std::stringbuf ordinary(expected + "unread tail");
                std::istream input(borrowed ? static_cast<std::streambuf*>(&decoded) : &ordinary);
                std::string observed;
                std::vector<std::size_t> checkpoints;
                bool interrupted = false;
                try {
                    flitzzip::consume_exact_stream_bytes(
                        input, boundary.bytes,
                        [&](std::span<const char> window) { observed.append(window.data(), window.size()); },
                        "final checkpoint boundary",
                        [&] {
                            checkpoints.push_back(observed.size());
                            if (fail_final && observed.size() == boundary.bytes) {
                                throw ContainerProgressAbort{};
                            }
                        });
                } catch (const ContainerProgressAbort&) {
                    interrupted = true;
                }
                REQUIRE_EQ(interrupted, fail_final && boundary.bytes != 0U);
                REQUIRE_EQ(checkpoints, boundary.checkpoints);
                REQUIRE_EQ(observed, expected);
                if (boundary.bytes == 0U) {
                    decoded.source_failure = true;
                    REQUIRE_EQ(decoded.consume(0U, [](std::span<const char>) { throw ContainerProgressAbort{}; }), 0U);
                    REQUIRE_EQ(decoded.underflow_calls, 0U);
                    decoded.source_failure = false;
                }
                const std::string suffix{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                REQUIRE_EQ(suffix, "unread tail");
            }
        }
    }
}

// Filtered inputs must remain cancellable while validation is reading payload,
// before any destination work. Plain metadata seeks need an initial checkpoint.
TEST_CASE(container_extraction_validation_is_observable_and_cancellable) {
    for (const auto& fixture : container_progress_cases()) {
        flitzzip_test::ScopedTestDirectory cleanup{
            test_temp_dir(std::string("container-validation-") + fixture.extension)};
        const auto& root = cleanup.path;
        const auto source = root / "payload.bin";
        const auto archive = root / (std::string("input.") + fixture.extension);
        const auto destination = root / "output";
        const std::string bytes(512U * 1024U + 17U, 'V');
        std::ofstream(source, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        fixture.create(source, archive);
        unsigned int checks = 0;
        const unsigned int abort_check = fixture.filtered ? 9U : 1U;
        bool aborted = false;
        try {
            (void)fixture.extract(archive, destination, false, [&](const flitzzip::ProgressSnapshot& snapshot) {
                if (snapshot.note == "Validating") {
                    REQUIRE_EQ(snapshot.total_bytes, 0U);
                    REQUIRE_EQ(snapshot.processed_bytes, 0U);
                    REQUIRE_EQ(snapshot.completed_entries, 0U);
                    if (++checks == abort_check) {
                        throw ContainerProgressAbort{};
                    }
                }
            });
        } catch (const ContainerProgressAbort&) {
            aborted = true;
        }
        REQUIRE_TRUE(aborted && checks == abort_check);
        REQUIRE_TRUE(!std::filesystem::exists(destination));
        const auto renamed = root / "released.archive";
        std::filesystem::rename(archive, renamed);
        std::filesystem::rename(renamed, archive);
    }
}

TEST_CASE(container_extraction_member_checkpoints_preserve_target_and_recover) {
    for (const auto& fixture : container_progress_cases()) {
        flitzzip_test::ScopedTestDirectory cleanup{test_temp_dir(std::string("container-copy-") + fixture.extension)};
        const auto& root = cleanup.path;
        const auto source = root / "payload.bin";
        const auto archive = root / (std::string("input.") + fixture.extension);
        const auto destination = root / "output";
        const auto target = destination / "payload.bin";
        const std::string bytes(512U * 1024U + 17U, 'P');
        std::ofstream(source, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        fixture.create(source, archive);
        std::filesystem::create_directory(destination);
        for (const int phase : {0, 1, 2}) {
            std::ofstream(target, std::ios::binary) << "existing payload";
            const auto original_time = std::filesystem::last_write_time(target);
            unsigned int final_checks = 0;
            bool aborted = false;
            try {
                (void)fixture.extract(archive, destination, true, [&](const flitzzip::ProgressSnapshot& snapshot) {
                    if (!snapshot.note.empty() || snapshot.processed_bytes == 0U) {
                        return;
                    }
                    const bool partial = snapshot.processed_bytes < snapshot.total_bytes;
                    if (!partial) {
                        ++final_checks;
                    }
                    if ((phase == 0 && partial) || (phase == 1 && final_checks == 1U) ||
                        (phase == 2 && final_checks == 2U)) {
                        throw ContainerProgressAbort{};
                    }
                });
            } catch (const ContainerProgressAbort&) {
                aborted = true;
            }
            REQUIRE_TRUE(aborted);
            REQUIRE_EQ(read_stream_fixture(target), "existing payload");
            REQUIRE_EQ(std::filesystem::last_write_time(target), original_time);
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator()),
                1);
            const auto renamed = root / "released.archive";
            std::filesystem::rename(archive, renamed);
            std::filesystem::rename(renamed, archive);
        }
        std::uint64_t observed = 0;
        const auto stats = fixture.extract(archive, destination, true, [&](const flitzzip::ProgressSnapshot& snapshot) {
            if (snapshot.note == "Validating") {
                REQUIRE_EQ(snapshot.processed_bytes, 0U);
                return;
            }
            REQUIRE_EQ(snapshot.total_bytes, bytes.size());
            REQUIRE_TRUE(snapshot.processed_bytes >= observed && snapshot.processed_bytes <= bytes.size());
            REQUIRE_TRUE(snapshot.processed_bytes - observed <= 65536U);
            observed = snapshot.processed_bytes;
        });
        REQUIRE_EQ(observed, bytes.size());
        REQUIRE_EQ(stats.output_bytes, bytes.size());
        REQUIRE_EQ(read_stream_fixture(target), bytes);
    }
}

// Failed/throwing sinks retain source position, including partial writes;
// prefailed sinks and zero requests must not decode or acquire a window.
TEST_CASE(decoded_input_buffer_sink_failures_preserve_source_and_recovery) {
    DecodedWindowFixture source("abcdefg");
    std::ostringstream invalid;
    invalid.setstate(std::ios::badbit);
    REQUIRE_EQ(source.transfer_to(invalid, 0U), 0U);
    bool rejected = false;
    try {
        (void)source.transfer_to(invalid, 1U);
    } catch (const std::ios_base::failure&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(source.underflow_calls, 0U);
    DecodedSinkFixture partial;
    partial.short_write = true;
    std::ostream short_output(&partial);
    rejected = false;
    try {
        (void)source.transfer_to(short_output, 4U);
    } catch (const std::ios_base::failure&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(partial.written, "abc");
    REQUIRE_EQ(source.consumed(), 0U);
    DecodedSinkFixture throwing;
    throwing.throw_on_write = true;
    std::ostream throwing_output(&throwing);
    throwing_output.exceptions(std::ios::badbit);
    rejected = false;
    try {
        (void)source.transfer_to(throwing_output, 4U);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()) == "independent sink failure";
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(source.consumed(), 0U);
    std::ostringstream recovered;
    REQUIRE_EQ(source.transfer_to(recovered, 7U), 7U);
    REQUIRE_EQ(recovered.str(), "abcdefg");
}

// Direct transfer retains the same owned get area for all putback characters;
// a provider exception remains distinguishable from successful EOF.
TEST_CASE(decoded_input_buffer_preserves_putback_and_source_errors) {
    DecodedWindowFixture source("abcdefg");
    std::ostringstream output;
    REQUIRE_EQ(source.transfer_to(output, 7U), 7U);
    std::istream input(&source);
    for (unsigned int index = 0U; index < 7U; ++index) {
        input.unget();
        REQUIRE_TRUE(input.good());
    }
    std::string restored(7U, '\0');
    input.read(restored.data(), static_cast<std::streamsize>(restored.size()));
    REQUIRE_EQ(restored, "abcdefg");
    source.source_failure = true;
    bool rejected = false;
    try {
        (void)source.transfer_to(output, 1U);
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string_view(error.what()) == "independent source failure";
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(output.str(), "abcdefg");
    UnpublishedDecodedWindow invalid;
    rejected = false;
    try {
        (void)invalid.transfer_to(output, 1U);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(output.str(), "abcdefg");
}

// Validate each actual codec facade and production standalone extraction path.
TEST_CASE(compression_stream_gzip_direct_chunk_transfer) {
    require_decoded_chunk_transfer<flitzzip::GzipOutputStream, flitzzip::GzipInputStream>("gz",
                                                                                          flitzzip::extract_gzip_file);
}

// Validate each actual codec facade and production standalone extraction path.
TEST_CASE(compression_stream_bzip2_direct_chunk_transfer) {
    require_decoded_chunk_transfer<flitzzip::Bzip2OutputStream, flitzzip::Bzip2InputStream>(
        "bz2", flitzzip::extract_bzip2_file);
}

// Validate each actual codec facade and production standalone extraction path.
TEST_CASE(compression_stream_zstd_direct_chunk_transfer) {
    require_decoded_chunk_transfer<flitzzip::ZstdOutputStream, flitzzip::ZstdInputStream>("zst",
                                                                                          flitzzip::extract_zstd_file);
}

// Purpose: Validate asynchronous ownership, finalization, wire determinism, and bounded codec allocation.
// Inputs: A 32 MiB repeated-record fixture written whole and with immediately reused fragment buffers.
// Outputs: Requires identical archives, exact decode, checked worker telemetry, and complete workspace release.
TEST_CASE(compression_stream_zstd_threaded_roundtrip) {
    const auto root = test_temp_dir("zstd-threaded");
    const auto record = make_stream_partition_fixture();
    constexpr std::size_t length = 32U * 1024U * 1024U;
    std::string input;
    input.reserve(length);
    while (input.size() < length) {
        input.append(record, 0U, std::min(record.size(), length - input.size()));
    }
    std::string first_encoded;
    for (const bool fragmented : {false, true}) {
        const auto path = root / (fragmented ? "fragmented.zst" : "whole.zst");
        flitzzip::ZstdOutputStream output(path, 5, input.size());
        output.exceptions(std::ios::badbit | std::ios::failbit);
        const auto expected_workers =
            flitzzip::zstd_stream_worker_count(input.size(), 5, std::thread::hardware_concurrency());
        REQUIRE_EQ(output.compression_workers(), expected_workers);
        REQUIRE_EQ(output.completed_workspace_bytes(), 0U);
        for (std::size_t offset = 0; offset < input.size();) {
            const auto count = std::min(input.size() - offset, fragmented ? 8191U : input.size());
            auto scratch = input.substr(offset, count);
            output.write(scratch.data(), static_cast<std::streamsize>(scratch.size()));
            std::fill(scratch.begin(), scratch.end(), static_cast<char>(0xA5));
            offset += count;
        }
        if (expected_workers != 0U) {
            bool rejected = false;
            try {
                (void)output.workspace_bytes();
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
        output.close();
        output.close();
        REQUIRE_EQ(output.workspace_bytes(), 0U);
        REQUIRE_TRUE(output.completed_workspace_bytes() > 0U);
        REQUIRE_TRUE(output.completed_workspace_bytes() < 256U * 1024U * 1024U);
        REQUIRE_EQ(output.input_bytes(), input.size());
        REQUIRE_EQ(output.output_bytes(), std::filesystem::file_size(path));
        const auto encoded = read_stream_fixture(path);
        if (!fragmented) {
            first_encoded = encoded;
        } else {
            REQUIRE_EQ(encoded, first_encoded);
        }
        flitzzip::ZstdInputStream decoded(path);
        std::array<char, 65536U> buffer{};
        std::size_t offset = 0U;
        while (decoded.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || decoded.gcount() > 0) {
            const auto count = static_cast<std::size_t>(decoded.gcount());
            REQUIRE_TRUE(offset <= input.size() && count <= input.size() - offset);
            REQUIRE_TRUE(std::equal(buffer.begin(), buffer.begin() + count, input.begin() + offset));
            offset += count;
        }
        decoded.finish();
        REQUIRE_EQ(offset, input.size());
    }
    std::filesystem::remove_all(root);
}
