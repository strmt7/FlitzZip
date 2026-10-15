#include "test_util.hpp"

#include "ar/ar_adapter.hpp"
#include "base64/base64_adapter.hpp"
#include "bzip2/bzip2_adapter.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "unix_compress/unix_compress_adapter.hpp"
#include "uue/uue_adapter.hpp"
#include "xxe/xxe_adapter.hpp"
#include "zip/zip_adapter.hpp"
#include "zstd/zstd_adapter.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>

namespace {

using Path = std::filesystem::path;
using Create = flitzzip::OperationStats (*)(const std::vector<Path>&, const Path&, const flitzzip::ProgressCallback&);
using Extract = flitzzip::OperationStats (*)(const Path&, const Path&, bool, const flitzzip::ProgressCallback&);
struct Adapter {
    const char* suffix;
    Create create;
    Extract extract;
};
constexpr std::array adapters{
    Adapter{".b64", flitzzip::compress_base64, flitzzip::extract_base64_file},
    Adapter{".uue", flitzzip::compress_uue, flitzzip::extract_uue_file},
    Adapter{".xxe", flitzzip::compress_xxe, flitzzip::extract_xxe_file},
    Adapter{".Z", flitzzip::compress_unix_compress, flitzzip::extract_unix_compress_file},
};

// Purpose: Select ordinary effort for public compatibility writers in the publication matrix.
// Inputs: Owned source/destination and synchronous callback. Outputs: Production writer result, without retries.
template <auto Writer>
flitzzip::OperationStats create_at_default_effort(const std::vector<Path>& sources, const Path& archive,
                                                  const flitzzip::ProgressCallback& progress) {
    return Writer(sources, archive, flitzzip::kDefaultCompressionLevel, progress);
}

// Purpose: Select ZIP verification explicitly when testing its transaction boundary.
// Inputs: Owned source/destination and synchronous callback. Outputs: Verified production ZIP writer result.
flitzzip::OperationStats create_verified_zip(const std::vector<Path>& sources, const Path& archive,
                                             const flitzzip::ProgressCallback& progress) {
    return flitzzip::compress_zip(sources, archive, flitzzip::kDefaultCompressionLevel, progress, true);
}

constexpr std::array publication_adapters{
    adapters[0],
    adapters[1],
    adapters[2],
    adapters[3],
    Adapter{".tar", flitzzip::compress_tar, flitzzip::extract_tar},
    Adapter{".ar", flitzzip::compress_ar, flitzzip::extract_ar},
    Adapter{".cpio", flitzzip::compress_cpio, flitzzip::extract_cpio},
    Adapter{".tar.gz", create_at_default_effort<flitzzip::compress_tar_gzip>, flitzzip::extract_tar_gzip},
    Adapter{".tar.bz2", create_at_default_effort<flitzzip::compress_tar_bzip2>, flitzzip::extract_tar_bzip2},
    Adapter{".tar.zst", create_at_default_effort<flitzzip::compress_tar_zstd>, flitzzip::extract_tar_zstd},
    Adapter{".cpio.gz", create_at_default_effort<flitzzip::compress_cpio_gzip>, flitzzip::extract_cpio_gzip},
    Adapter{".gz", create_at_default_effort<flitzzip::compress_gzip>, flitzzip::extract_gzip_file},
    Adapter{".bz2", create_at_default_effort<flitzzip::compress_bzip2>, flitzzip::extract_bzip2_file},
    Adapter{".zst", create_at_default_effort<flitzzip::compress_zstd>, flitzzip::extract_zstd_file},
    Adapter{".zip", create_verified_zip, flitzzip::extract_zip},
};

// Purpose: Own only a generated test directory, including cleanup after failed assertions.
// Inputs: A stable test label. Outputs: Isolated fixtures whose removal never follows an external path.
struct Fixture {
    Path root;
    // Purpose: Create the test-owned root. Inputs: Stable label. Outputs: Empty temporary directory.
    explicit Fixture(const char* label) : root(test_temp_dir(label)) {}
    // Purpose: Remove closed test fixtures on scope exit. Inputs: Owned root. Outputs: Best-effort cleanup.
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// Purpose: Write a bounded fixture independently of the adapters under test.
// Inputs: Owned path and exact bytes. Outputs: Closed binary file; I/O failure fails the test.
void put(const Path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

// Purpose: Read all bytes of a small test fixture for independent preservation assertions.
// Inputs: Owned regular file. Outputs: Exact bytes, or a test failure on I/O errors.
std::string get(const Path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.is_open());
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    REQUIRE_TRUE(!input.bad());
    return bytes;
}

// Purpose: Detect leaked staging directories as well as unintended files after rejection.
// Inputs: Existing owned root and expected immediate children. Outputs: Exact inventory assertion.
void require_children(const Path& root, std::initializer_list<Path> expected) {
    std::vector<Path> actual;
    for (const auto& child : std::filesystem::directory_iterator(root)) {
        actual.push_back(child.path().filename());
    }
    std::vector<Path> wanted(expected);
    std::ranges::sort(actual);
    std::ranges::sort(wanted);
    REQUIRE_EQ(actual, wanted);
}

// Purpose: Assert domain rejection without accepting unrelated exceptions as success.
// Inputs: A synchronous operation. Outputs: Requires an archive/security error from that operation.
template <typename Operation> void require_rejected(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Prove malformed text cannot publish partial data or destroy an existing destination.
// Inputs: Decoder, bounded invalid fixtures and an independent valid fixture decoding to foo.
// Outputs: Both overwrite states reject all cases, clean staging, and permit valid reuse afterward.
void require_malformed_text(const Adapter& adapter, const std::vector<std::string>& invalid, std::string_view valid) {
    Fixture fixture("transfer-malformed");
    const auto archive = fixture.root / (std::string("payload.bin") + adapter.suffix);
    const auto destination = fixture.root / "out";
    std::filesystem::create_directory(destination);
    for (const bool existing : {false, true}) {
        for (std::size_t index = 0; index < invalid.size(); ++index) {
            put(archive, invalid[index]);
            if (existing) {
                put(destination / "payload.bin", "keep original");
            }
            try {
                require_rejected([&] { adapter.extract(archive, destination, true, {}); });
                if (existing) {
                    REQUIRE_EQ(get(destination / "payload.bin"), "keep original");
                    require_children(destination, {"payload.bin"});
                } else {
                    require_children(destination, {});
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(adapter.suffix) + " malformed case " + std::to_string(index) +
                                         ": " + error.what());
            }
        }
    }
    put(archive, valid);
    const auto stats = adapter.extract(archive, destination, true, {});
    REQUIRE_EQ(stats.output_bytes, 3U);
    REQUIRE_EQ(get(destination / "payload.bin"), "foo");
    require_children(destination, {"payload.bin"});
}

}  // namespace

// Purpose: Reject malformed Base64 padding, metadata and trailers without destructive publication.
// Inputs: Independent text mutations, including metadata limits and the alternate encoded filename wrapper.
// Outputs: Every invalid case preserves its destination, followed by a successful independently encoded control.
TEST_CASE(base64_failure_matrix_preserves_destinations) {
    const std::string header = "begin-base64 644 payload.bin\n";
    require_malformed_text(adapters[0],
                           {"!", "Z", "Zm", "Zm9", "=m9v", "Z=9v", "Zg=A", "Zg==A", "Zm9v!", "begin-base64 644\n",
                            "begin-base64  payload.bin\n", "begin-base64 644 \n", "begin-base64 648 payload.bin\n",
                            "begin-base64-encoded 644 !\n", header + "Zm9v\n", header + "Zm9v\n====\ntrailing\n",
                            header + std::string(4097, 'A'), std::string(128, '\n') + header},
                           "begin-base64-encoded 644 cGF5bG9hZC5iaW4=\n Z m9v\t\n====\n \t\r\n");
}

// Purpose: Reject incomplete or overlong UUencode records, invalid modes and post-terminator content.
// Inputs: Handwritten foo payload and mutations of each framing layer. Outputs: No partial publication or debris.
TEST_CASE(uue_failure_matrix_preserves_destinations) {
    const std::string header = "begin 644 payload.bin\n";
    require_malformed_text(adapters[1],
                           {"", "mail preamble\n", "begin 644\n", "begin  payload.bin\n", "begin 644 \n",
                            "begin 648 payload.bin\n", std::string(128, '\n') + header, header + "\n", header + "N\n",
                            header + "#9F]\n", header + "#9F]O!\n", header + "#9F]a\n", header + "#9F]O\n",
                            header + "#9F]O\n`\n", header + "#9F]O\n`\nwrong\n", header + "#9F]O\n`\nend\nextra\n",
                            header + std::string(4097, '#')},
                           "mail preamble\r\nbegin 644 payload.bin\r\n#9F]O \t\r\n`\r\nend\r\n \t\r\n");
}

// Purpose: Reject invalid XXEncode length fields, terminators and wrappers before replacing user bytes.
// Inputs: Handwritten foo payload and distinct framing failures. Outputs: Rejection with complete cleanup/reuse.
TEST_CASE(xxe_failure_matrix_preserves_destinations) {
    const std::string header = "begin 644 payload.bin\n";
    require_malformed_text(adapters[2],
                           {"", "mail preamble\n", "begin 644\n", "begin  payload.bin\n", "begin 644 \n",
                            "begin 648 payload.bin\n", std::string(128, '\n') + header, header + "\n", header + "i\n",
                            header + "1Nax\n", header + "1Naxj!\n", header + "1Nax!\n", header + "+x\n",
                            header + " x\n", header + "1Naxj\n", header + "1Naxj\n+\n", header + "1Naxj\n+\nwrong\n",
                            header + "1Naxj\n+\nend\nextra\n", header + std::string(4097, '1')},
                           "mail preamble\r\nbegin 644 payload.bin\r\n1Naxj \t\r\n+ \t\r\nend\r\n \t\r\n");
}

// Purpose: Preserve canonical empty and short Base64 inputs across wrapper and raw modes.
// Inputs: Independent RFC-style vectors with zero, one, two and three decoded bytes.
// Outputs: Byte-exact extraction, correct sizes and no staging artifacts.
TEST_CASE(base64_independent_short_and_empty_vectors) {
    Fixture fixture("base64-short-vectors");
    const auto archive = fixture.root / "payload.bin.b64";
    const auto destination = fixture.root / "out";
    for (const auto& [encoded, decoded] : std::array<std::pair<const char*, const char*>, 4>{
             {{"", ""}, {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"}}}) {
        for (const bool wrapped : {false, true}) {
            put(archive, wrapped ? std::string("\nbegin-base64 644 payload.bin\n") + encoded + "\n====\n" : encoded);
            const auto stats = flitzzip::extract_base64_file(archive, destination, true);
            REQUIRE_EQ(get(destination / "payload.bin"), decoded);
            REQUIRE_EQ(stats.output_bytes, std::string_view(decoded).size());
            require_children(destination, {"payload.bin"});
        }
    }
}

// Purpose: Validate source cardinality, regular-file admission and same-inode overwrite protection.
// Inputs: Every transfer adapter, empty/multiple/missing/directory inputs and a real hard-link alias.
// Outputs: Invalid operations preserve both source bytes and an existing archive with no staging leakage.
TEST_CASE(transfer_creation_rejects_source_aliases_and_invalid_shapes) {
    for (const auto& adapter : adapters) {
        Fixture fixture("transfer-source-shapes");
        const auto source = fixture.root / "payload.bin";
        const auto archive = fixture.root / "archive";
        const auto directory = fixture.root / "directory";
        const auto alias = fixture.root / "alias";
        put(source, "source bytes");
        put(archive, "existing archive");
        std::filesystem::create_directory(directory);
        std::filesystem::create_hard_link(source, alias);
        for (const std::vector<Path>& sources :
             {std::vector<Path>{}, {source, alias}, {directory}, {fixture.root / "missing"}}) {
            require_rejected([&] { adapter.create(sources, archive, {}); });
            REQUIRE_EQ(get(archive), "existing archive");
        }
        for (const auto& target : {source, alias}) {
            require_rejected([&] { adapter.create({source}, target, {}); });
            REQUIRE_EQ(get(source), "source bytes");
            REQUIRE_EQ(get(alias), "source bytes");
        }
        require_children(fixture.root, {"payload.bin", "archive", "directory", "alias"});
    }
}

// Purpose: Ensure callback exceptions during actual encoding cancel staging and propagate unchanged.
// Inputs: Four adapters and a small binary source, with both new and existing archive destinations.
// Outputs: No published partial archive, no staging debris, and byte-exact successful reuse after cancellation.
TEST_CASE(transfer_creation_cancellation_preserves_destination_and_reuses) {
    struct Cancel {};
    for (const auto& adapter : adapters) {
        Fixture fixture("transfer-create-cancel");
        const auto source = fixture.root / "payload.bin";
        const auto archive = fixture.root / (std::string("payload.bin") + adapter.suffix);
        const std::string payload(4096, 'q');
        put(source, payload);
        for (const bool existing : {false, true}) {
            if (existing) {
                put(archive, "keep original");
            }
            bool cancelled = false;
            try {
                adapter.create({source}, archive, [](const flitzzip::ProgressSnapshot& snapshot) {
                    if (snapshot.processed_bytes > 0 && snapshot.completed_entries == 0) {
                        throw Cancel{};
                    }
                });
            } catch (const Cancel&) {
                cancelled = true;
            }
            REQUIRE_TRUE(cancelled);
            REQUIRE_EQ(std::filesystem::exists(archive), existing);
            if (existing) {
                REQUIRE_EQ(get(archive), "keep original");
                require_children(fixture.root, {source.filename(), archive.filename()});
            } else {
                require_children(fixture.root, {source.filename()});
            }
        }
        adapter.create({source}, archive, {});
        const auto destination = fixture.root / "out";
        adapter.extract(archive, destination, false, {});
        REQUIRE_EQ(get(destination / "payload.bin"), payload);
        require_children(destination, {"payload.bin"});
    }
}

// Purpose: Exercise actual Windows replacement denial after encoding and decoding have completed.
// Inputs: Test-owned final files held without delete sharing; all handles close before valid reuse.
// Outputs: Old destination bytes survive, temporary directories disappear, and both operations recover normally.
TEST_CASE(archive_publication_denial_is_atomic_and_recoverable) {
    for (const auto& adapter : publication_adapters) {
        Fixture fixture("transfer-publication-denial");
        const auto source = fixture.root / "payload.bin";
        const auto archive = fixture.root / (std::string("payload.bin") + adapter.suffix);
        const auto destination = fixture.root / "out";
        put(source, "new payload");
        put(archive, "old archive");
        {
            const std::unique_ptr<void, decltype(&CloseHandle)> pin(
                CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr),
                &CloseHandle);
            REQUIRE_TRUE(pin.get() != INVALID_HANDLE_VALUE);
            require_rejected([&] { adapter.create({source}, archive, {}); });
            REQUIRE_EQ(get(archive), "old archive");
            require_children(fixture.root, {source.filename(), archive.filename()});
        }
        adapter.create({source}, archive, {});
        std::filesystem::create_directory(destination);
        const auto target = destination / "payload.bin";
        put(target, "old target");
        {
            const std::unique_ptr<void, decltype(&CloseHandle)> pin(
                CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr),
                &CloseHandle);
            REQUIRE_TRUE(pin.get() != INVALID_HANDLE_VALUE);
            require_rejected([&] { adapter.extract(archive, destination, true, {}); });
            REQUIRE_EQ(get(target), "old target");
            require_children(destination, {"payload.bin"});
        }
        adapter.extract(archive, destination, true, {});
        REQUIRE_EQ(get(target), "new payload");
        require_children(destination, {"payload.bin"});
    }
}
