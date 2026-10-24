#include "test_util.hpp"

#include "core/integrity.hpp"
#include "core/result.hpp"
#include "core/sha256_provider.hpp"
#include "core/path_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
namespace {

enum class CngFault { None, Open, Property, Create, FirstUpdate, SecondUpdate, Finish };

struct CngCalls {
    unsigned opens = 0;
    unsigned properties = 0;
    unsigned creates = 0;
    unsigned updates = 0;
    unsigned finishes = 0;
    unsigned destroys = 0;
    unsigned closes = 0;
    std::array<std::size_t, 3> update_sizes{};
    bool valid = true;
};

// Purpose: Refuse one CNG operation while every successful acquisition and release uses the production provider.
// Inputs: A synchronous per-instance fault; borrowed input and object storage remain owned by the tested hasher.
// Outputs: Real-resource observations, release results and order, without global hooks or resource exhaustion.
class RecordingCngProvider final : public flitzzip::integrity_detail::CngSha256Provider {
  public:
    CngFault fault = CngFault::None;
    mutable CngCalls calls;

    // Purpose: Observe algorithm acquisition; inputs: empty output; outputs: real provider or selected refusal.
    bool open_algorithm(void*& algorithm) const noexcept override {
        ++calls.opens;
        calls.valid = calls.valid && algorithm == nullptr && algorithm_ == nullptr && hash_ == nullptr;
        if (fault == CngFault::Open)
            return false;
        const bool ok = CngSha256Provider::open_algorithm(algorithm);
        algorithm_ = algorithm;
        calls.valid = calls.valid && ok && algorithm_ != nullptr;
        return ok;
    }

    // Purpose: Observe storage requirements; inputs: retained algorithm; outputs: native length or selected refusal.
    bool object_length(void* algorithm, std::uint32_t& bytes) const noexcept override {
        ++calls.properties;
        calls.valid = calls.valid && algorithm == algorithm_ && algorithm != nullptr;
        return fault != CngFault::Property && CngSha256Provider::object_length(algorithm, bytes);
    }

    // Purpose: Observe hash acquisition; inputs: live provider/storage; outputs: real owned hash or refusal.
    bool create_hash(void* algorithm, void*& hash, std::span<std::byte> object) const noexcept override {
        ++calls.creates;
        calls.valid =
            calls.valid && algorithm == algorithm_ && algorithm != nullptr && hash == nullptr && !object.empty();
        if (fault == CngFault::Create)
            return false;
        const bool ok = CngSha256Provider::create_hash(algorithm, hash, object);
        hash_ = hash;
        calls.valid = calls.valid && ok && hash_ != nullptr;
        return ok;
    }

    // Purpose: Observe bounded stream updates; inputs: live hash and stable bytes; outputs: native result or refusal.
    bool update_hash(void* hash, std::span<const std::byte> bytes) const noexcept override {
        ++calls.updates;
        calls.valid = calls.valid && hash == hash_ && hash != nullptr && !bytes.empty() && bytes.size() <= 1048576U;
        if (calls.updates <= calls.update_sizes.size())
            calls.update_sizes[calls.updates - 1U] = bytes.size();
        else
            calls.valid = false;
        if ((fault == CngFault::FirstUpdate && calls.updates == 1U) ||
            (fault == CngFault::SecondUpdate && calls.updates == 2U))
            return false;
        return CngSha256Provider::update_hash(hash, bytes);
    }

    // Purpose: Observe finalization; inputs: live hash and exact digest extent; outputs: native bytes or refusal.
    bool finish_hash(void* hash, std::span<std::byte, 32> digest) const noexcept override {
        ++calls.finishes;
        calls.valid = calls.valid && hash == hash_ && hash != nullptr;
        return fault != CngFault::Finish && CngSha256Provider::finish_hash(hash, digest);
    }

    // Purpose: Verify real hash destruction precedes provider release; inputs: owned hash; outputs: native status.
    bool destroy_hash(void* hash) const noexcept override {
        ++calls.destroys;
        if (hash != hash_ || hash == nullptr || algorithm_ == nullptr) {
            calls.valid = false;
            return false;
        }
        const bool ok = CngSha256Provider::destroy_hash(hash);
        calls.valid = calls.valid && ok;
        if (ok)
            hash_ = nullptr;
        return ok;
    }

    // Purpose: Verify actual provider release follows hash destruction; inputs: owned algorithm; outputs: status.
    bool close_algorithm(void* algorithm) const noexcept override {
        ++calls.closes;
        if (algorithm != algorithm_ || algorithm == nullptr || hash_ != nullptr) {
            calls.valid = false;
            return false;
        }
        const bool ok = CngSha256Provider::close_algorithm(algorithm);
        calls.valid = calls.valid && ok;
        if (ok)
            algorithm_ = nullptr;
        return ok;
    }

    // Purpose: Observe ownership after success/failure; inputs: none; outputs: true only after all real releases.
    bool empty() const noexcept {
        return algorithm_ == nullptr && hash_ == nullptr;
    }

  private:
    mutable void* algorithm_ = nullptr;
    mutable void* hash_ = nullptr;
};

}  // namespace

// Purpose: Prove every CNG failure propagates without a partial digest, leaked reference or poisoned reusable provider.
// Inputs: Real CNG contexts, deterministic multi-chunk bytes and independent failure-stage expectations.
// Outputs: Exact diagnostics, bounded updates, successful native releases, unchanged bytes and valid reuse.
TEST_CASE(sha256_provider_failures_release_partial_construction_and_recover) {
    std::vector<std::byte> bytes(2U * 1048576U + 17U);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(index % 251U);
    const auto original = bytes;
    const auto expected = flitzzip::sha256_bytes(bytes);
    struct Case {
        CngFault fault;
        const char* diagnostic;
        unsigned properties, creates, updates, finishes, destroys, closes;
    };
    constexpr std::array<Case, 6> cases{{
        {CngFault::Open, "cannot open Windows SHA-256 provider", 0, 0, 0, 0, 0, 0},
        {CngFault::Property, "cannot query Windows SHA-256 object length", 1, 0, 0, 0, 0, 1},
        {CngFault::Create, "cannot create Windows SHA-256 hash", 1, 1, 0, 0, 0, 1},
        {CngFault::FirstUpdate, "Windows SHA-256 update failed", 1, 1, 1, 0, 1, 1},
        {CngFault::SecondUpdate, "Windows SHA-256 update failed", 1, 1, 2, 0, 1, 1},
        {CngFault::Finish, "Windows SHA-256 finalize failed", 1, 1, 3, 1, 1, 1},
    }};
    for (const auto& entry : cases) {
        RecordingCngProvider provider;
        provider.fault = entry.fault;
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::integrity_detail::sha256_bytes_with_provider(bytes, provider));
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string(error.what()) == entry.diagnostic;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(provider.empty() && provider.calls.valid);
        REQUIRE_EQ(provider.calls.opens, 1U);
        REQUIRE_EQ(provider.calls.properties, entry.properties);
        REQUIRE_EQ(provider.calls.creates, entry.creates);
        REQUIRE_EQ(provider.calls.updates, entry.updates);
        REQUIRE_EQ(provider.calls.finishes, entry.finishes);
        REQUIRE_EQ(provider.calls.destroys, entry.destroys);
        REQUIRE_EQ(provider.calls.closes, entry.closes);
        REQUIRE_EQ(bytes, original);
        provider.fault = CngFault::None;
        provider.calls = {};
        REQUIRE_EQ(flitzzip::integrity_detail::sha256_bytes_with_provider(bytes, provider), expected);
        REQUIRE_TRUE(provider.empty() && provider.calls.valid);
        REQUIRE_EQ(provider.calls.updates, 3U);
        REQUIRE_EQ(provider.calls.update_sizes, (std::array<std::size_t, 3>{1048576, 1048576, 17}));
        REQUIRE_EQ(provider.calls.finishes, 1U);
        REQUIRE_EQ(provider.calls.destroys, 1U);
        REQUIRE_EQ(provider.calls.closes, 1U);
        REQUIRE_EQ(bytes, original);
    }
}

// Purpose: Keep empty-snapshot hashing valid without passing a zero-sized buffer into CNG updates.
// Inputs: Empty borrowed snapshot and real provider. Outputs: Standard empty digest and complete native cleanup.
TEST_CASE(sha256_provider_empty_snapshot_finalizes_without_updates) {
    RecordingCngProvider provider;
    REQUIRE_EQ(flitzzip::integrity_detail::sha256_bytes_with_provider({}, provider), flitzzip::sha256_bytes({}));
    REQUIRE_TRUE(provider.empty() && provider.calls.valid);
    REQUIRE_EQ(provider.calls.updates, 0U);
    REQUIRE_EQ(provider.calls.finishes, 1U);
    REQUIRE_EQ(provider.calls.destroys, 1U);
    REQUIRE_EQ(provider.calls.closes, 1U);
}

// Purpose: Keep disabled integrity a no-op even when filesystem admission would reject the selected path.
// Inputs: A native path retaining an embedded NUL; both public hash entry points with integrity disabled.
// Outputs: Empty results without path resolution, provider work or a caller-visible exception.
TEST_CASE(sha256_provider_disabled_modes_skip_path_admission) {
    std::wstring name = L"invalid";
    name.push_back(L'\0');
    name += L"hidden";
    const std::filesystem::path path(name);
    const std::array results{flitzzip::hash_file(path, flitzzip::IntegrityMode::Disabled),
                             flitzzip::hash_path(path, flitzzip::IntegrityMode::Disabled)};
    for (const auto& result : results) {
        REQUIRE_TRUE(!result.attempted);
        REQUIRE_TRUE(result.algorithm.empty() && result.target.empty() && result.hex_digest.empty());
        REQUIRE_EQ(result.bytes_hashed, 0U);
        REQUIRE_EQ(result.files_hashed, 0U);
        REQUIRE_EQ(result.directories_hashed, 0U);
    }
}

// Purpose: Reject a directory through the file-only API with complete Unicode diagnostics and released OS locks.
// Inputs: An owned supplementary-Unicode directory followed by valid file hashing in the same directory.
// Outputs: Exact ArchiveError, successful rename after rejection and matching file/snapshot digests on reuse.
TEST_CASE(sha256_provider_file_api_rejects_directory_and_releases_its_lock) {
    const auto root = test_temp_dir("sha256-directory-admission");
    const auto directory = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1");
    std::filesystem::create_directory(directory);
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::hash_file(directory, flitzzip::IntegrityMode::Sha256));
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()),
                   "hash target is not a regular file: " + flitzzip::path_diagnostic_utf8(directory));
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(std::filesystem::is_empty(directory));
    const auto moved = root / L"released";
    REQUIRE_TRUE(!std::filesystem::exists(moved));
    std::filesystem::rename(directory, moved);
    std::filesystem::rename(moved, directory);
    const auto file = directory / L"valid.txt";
    const std::string payload = "CNG admission recovery";
    {
        std::ofstream output(file, std::ios::binary);
        output << payload;
        REQUIRE_TRUE(output.good());
    }
    const auto result = flitzzip::hash_file(file, flitzzip::IntegrityMode::Sha256);
    REQUIRE_TRUE(result.attempted);
    REQUIRE_EQ(result.bytes_hashed, payload.size());
    REQUIRE_EQ(result.hex_digest, flitzzip::sha256_bytes(std::as_bytes(std::span(payload.data(), payload.size()))));
}

// Purpose: Preserve the intended admission error when missing names cannot be represented in the host code page.
// Inputs: Both public hash APIs and one absent supplementary-Unicode filename under an owned directory.
// Outputs: Exact UTF-8 ArchiveError messages, no created objects and repeatable failure without retained handles.
TEST_CASE(sha256_provider_missing_unicode_source_preserves_admission_error) {
    const auto root = test_temp_dir("sha256-unicode-missing");
    const auto missing = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1.bin");
    const auto expected = "cannot open and lock integrity target: " + flitzzip::path_diagnostic_utf8(missing);
    for (const bool tree : {false, true, false, true}) {
        bool rejected = false;
        try {
            if (tree)
                static_cast<void>(flitzzip::hash_path(missing, flitzzip::IntegrityMode::Sha256));
            else
                static_cast<void>(flitzzip::hash_file(missing, flitzzip::IntegrityMode::Sha256));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), expected);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(std::filesystem::is_empty(root));
    }
}
#endif
