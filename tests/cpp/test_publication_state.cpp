#include "core/file_publish.hpp"
#include "core/result.hpp"
#include "core/windows_handle.hpp"
#include "test_util.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <system_error>
#include <windows.h>
#include <string>

namespace {

// Purpose: Read small publication fixtures independently of the archive implementation.
// Inputs: An existing ordinary test file. Outputs: Its exact bytes; throws if opening fails.
std::string publication_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.is_open());
    return {std::istreambuf_iterator<char>(input), {}};
}

struct PublicationCancelled {};

}  // namespace

// Purpose: Reject missing adapters before acquiring staging or invoking caller work.
// Inputs: Both publication policies, an absent adapter, and observable callbacks.
// Outputs: Rejects each request without filesystem side effects or callback invocation.
TEST_CASE(publication_state_requires_adapter_before_any_work) {
    const auto root = test_temp_dir("publication-no-adapter");
    for (const bool validate : {false, true}) {
        const auto target = root / (validate ? "validated" : "direct");
        int callbacks = 0;
        bool rejected = false;
        try {
            flitzzip::extract_with_publication(
                {.destination = target, .validate_before_publish = validate}, {}, [&](const auto&) { ++callbacks; },
                [&] { ++callbacks; });
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(callbacks, 0);
        REQUIRE_TRUE(!std::filesystem::exists(target));
    }
}

// Purpose: Ensure caller-mutated reservation fields cannot redirect cleanup or publication.
// Inputs: A real reservation, foreign canary directory, and copies with independently altered fields.
// Outputs: Keeps both the canary and original payload intact, then publishes the original successfully.
TEST_CASE(publication_state_rejects_forged_reservation_fields) {
    const auto root = test_temp_dir("publication-forged-reservation");
    const auto target = root / "final.txt";
    const auto canary = root / "foreign";
    std::filesystem::create_directory(canary);
    std::ofstream(canary / "keep.txt", std::ios::binary) << "unrelated";
    const auto original = flitzzip::reserve_file_publish_target(target);
    std::ofstream(original.file, std::ios::binary) << "verified";
    for (const int field : {0, 1, 2}) {
        auto forged = original;
        if (field == 0)
            forged.directory = canary;
        else if (field == 1)
            forged.file = canary / "keep.txt";
        else
            forged.state.reset();
        flitzzip::cleanup_file_publish_target(forged);
        bool rejected = false;
        try {
            flitzzip::commit_verified_file(forged, target, true);
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(publication_bytes(canary / "keep.txt"), "unrelated");
        REQUIRE_EQ(publication_bytes(original.file), "verified");
        REQUIRE_TRUE(!std::filesystem::exists(target));
    }
    flitzzip::cleanup_file_publish_target({});
    flitzzip::commit_verified_file(original, target, false);
    flitzzip::cleanup_file_publish_target(original);
    REQUIRE_EQ(publication_bytes(target), "verified");
    REQUIRE_EQ(publication_bytes(canary / "keep.txt"), "unrelated");
}

// Purpose: Keep a failed publication recoverable while preventing use after commit.
// Inputs: A transaction with no payload, followed by a complete payload and a repeated commit.
// Outputs: Preserves existing bytes on failure, publishes a complete replacement, and rejects the second commit.
TEST_CASE(publication_state_missing_payload_can_recover_once) {
    const auto root = test_temp_dir("publication-missing-payload");
    const auto target = root / "final.txt";
    std::ofstream(target, std::ios::binary) << "original";
    flitzzip::FilePublishTransaction transaction(target);
    bool rejected = false;
    try {
        transaction.commit(true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(publication_bytes(target), "original");
    std::ofstream(transaction.staging_path(), std::ios::binary) << "replacement";
    transaction.commit(true);
    REQUIRE_EQ(publication_bytes(target), "replacement");
    REQUIRE_TRUE(!std::filesystem::exists(transaction.staging_path().parent_path()));
    rejected = false;
    try {
        transaction.commit(true);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(publication_bytes(target), "replacement");
}

// Purpose: Refuse an alias of caller-owned bytes as a supposedly private publication payload.
// Inputs: A real staged hard link beside an existing target, followed by an ordinary staged file.
// Outputs: Preserves both caller files on refusal and publishes only the independently written retry.
TEST_CASE(publication_state_rejects_aliased_staging_payload_and_recovers) {
    const auto root = test_temp_dir("publication-aliased-payload");
    const auto outside = root / "outside.txt";
    const auto target = root / "final.txt";
    std::ofstream(outside, std::ios::binary) << "caller-owned";
    std::ofstream(target, std::ios::binary) << "original";

    flitzzip::FilePublishTransaction transaction(target);
    const auto stage = transaction.staging_path();
    std::filesystem::create_hard_link(outside, stage);
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 2U);

    bool rejected = false;
    try {
        transaction.commit(true);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(publication_bytes(target), "original");
    REQUIRE_EQ(publication_bytes(outside), "caller-owned");
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 2U);

    std::filesystem::remove(stage);
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 1U);
    std::ofstream(stage, std::ios::binary) << "verified replacement";
    transaction.commit(true);
    REQUIRE_EQ(publication_bytes(target), "verified replacement");
    REQUIRE_EQ(publication_bytes(outside), "caller-owned");
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 1U);
}

// Purpose: Ensure explicit cleanup invalidates every shared reservation owner permanently.
// Inputs: Two owners of one staging file, repeated cleanup, and a later attempted commit.
// Outputs: Removes private bytes exactly once, rejects the stale owner, and preserves the existing output.
TEST_CASE(publication_state_cleanup_expires_shared_owners) {
    const auto root = test_temp_dir("publication-expired-owner");
    const auto target = root / "final.txt";
    std::ofstream(target, std::ios::binary) << "original";
    const auto original = flitzzip::reserve_file_publish_target(target);
    const auto survivor = original;
    std::ofstream(original.file, std::ios::binary) << "private";
    flitzzip::cleanup_file_publish_target(original);
    flitzzip::cleanup_file_publish_target(survivor);
    bool rejected = false;
    try {
        flitzzip::commit_verified_file(survivor, target, true);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(original.directory));
    REQUIRE_EQ(publication_bytes(target), "original");
}

// Purpose: Exercise cancellation at every observable publication boundary and verify safe retry.
// Inputs: Two tiny staged files, one existing output, and a failure at each callback seen in a successful run.
// Outputs: Never exposes partial file bytes, cleans staging on unwind, and replaces all files on a fresh retry.
TEST_CASE(publication_state_cancellation_is_atomic_per_file_and_retryable) {
    const auto root = test_temp_dir("publication-cancellation-boundaries");
    int successful_checkpoints = 0;
    for (int failure = -1; failure < successful_checkpoints; ++failure) {
        const auto target = root / std::to_string(failure);
        std::filesystem::create_directories(target / "nested");
        std::ofstream(target / "nested" / "old.txt", std::ios::binary) << "old complete bytes";
        std::filesystem::path staging;
        int checkpoints = 0;
        bool cancelled = false;
        try {
            flitzzip::DirectoryPublishTransaction transaction(target);
            staging = transaction.staging_directory();
            std::filesystem::create_directory(staging / "nested");
            std::ofstream(staging / "nested" / "old.txt", std::ios::binary) << "new complete bytes";
            std::ofstream(staging / "added.txt", std::ios::binary) << "added complete bytes";
            transaction.publish(true, [&] {
                if (checkpoints++ == failure)
                    throw PublicationCancelled{};
            });
            bool repeated_rejected = false;
            try {
                transaction.publish(true);
            } catch (const flitzzip::ArchiveError&) {
                repeated_rejected = true;
            }
            REQUIRE_TRUE(repeated_rejected);
        } catch (const PublicationCancelled&) {
            cancelled = true;
        }
        REQUIRE_EQ(cancelled, failure >= 0);
        REQUIRE_TRUE(!std::filesystem::exists(staging));
        const auto old_bytes = publication_bytes(target / "nested" / "old.txt");
        REQUIRE_TRUE(old_bytes == "old complete bytes" || old_bytes == "new complete bytes");
        if (std::filesystem::exists(target / "added.txt"))
            REQUIRE_EQ(publication_bytes(target / "added.txt"), "added complete bytes");
        if (failure < 0) {
            successful_checkpoints = checkpoints;
            REQUIRE_TRUE(successful_checkpoints > 0);
        } else {
            REQUIRE_EQ(checkpoints, failure + 1);
            flitzzip::DirectoryPublishTransaction retry(target);
            std::filesystem::create_directory(retry.staging_directory() / "nested");
            std::ofstream(retry.staging_directory() / "nested" / "old.txt", std::ios::binary) << "retry complete";
            std::ofstream(retry.staging_directory() / "added.txt", std::ios::binary) << "retry added";
            retry.publish(true);
            REQUIRE_EQ(publication_bytes(target / "nested" / "old.txt"), "retry complete");
            REQUIRE_EQ(publication_bytes(target / "added.txt"), "retry added");
        }
    }
}

// Purpose: Refuse directory-only spellings before reserving a file publication beside the caller's directory.
// Inputs: An owned populated directory with an explicit trailing separator, then a normal file target.
// Outputs: No staging or caller changes after refusal; the same parent still supports normal publication.
TEST_CASE(publication_state_file_target_requires_a_filename_before_reservation) {
    const auto root = test_temp_dir("publication-directory-target");
    const auto directory = root / "nested";
    std::filesystem::create_directory(directory);
    std::ofstream(directory / "keep.txt", std::ios::binary) << "caller-owned";
    bool refused = false;
    try {
        const flitzzip::FilePublishTransaction invalid(directory / "");
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(publication_bytes(directory / "keep.txt"), "caller-owned");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}), 1);
    flitzzip::FilePublishTransaction valid(directory / "result.txt");
    std::ofstream(valid.staging_path(), std::ios::binary) << "verified result";
    valid.commit(false);
    REQUIRE_EQ(publication_bytes(directory / "result.txt"), "verified result");
    REQUIRE_EQ(publication_bytes(directory / "keep.txt"), "caller-owned");
}

// Purpose: Challenge actual inventory and publication transitions when a staged file disappears between checks.
// Inputs: Tiny private trees and deletion at each observable checkpoint, discovered from a successful control.
// Outputs: No caller data changes, no partial payload, at least one stale-inventory refusal and successful reuse.
TEST_CASE(publication_state_disappearing_quarantine_entries_preserve_caller_and_recover) {
    const auto root = test_temp_dir("publication-disappearing-entry");
    int successful_checkpoints = 0;
    int stale_inventory_refusals = 0;
    for (int mutation = -1; mutation < successful_checkpoints; ++mutation) {
        const auto destination = root / std::to_string(mutation);
        std::filesystem::create_directories(destination);
        std::ofstream(destination / "keep.txt", std::ios::binary) << "caller-owned";
        std::filesystem::path staging;
        int checkpoints = 0;
        bool removed = false;
        bool rejected = false;
        try {
            flitzzip::DirectoryPublishTransaction transaction(destination);
            staging = transaction.staging_directory();
            const auto payload = staging / "payload.txt";
            std::ofstream(payload, std::ios::binary) << "complete verified payload";
            transaction.publish(false, [&] {
                if (checkpoints++ == mutation) {
                    removed = std::filesystem::remove(payload);
                }
            });
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
            ++stale_inventory_refusals;
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        if (mutation < 0) {
            successful_checkpoints = checkpoints;
            REQUIRE_TRUE(successful_checkpoints > 1);
            REQUIRE_TRUE(!rejected);
        }
        REQUIRE_TRUE(!std::filesystem::exists(staging));
        REQUIRE_EQ(publication_bytes(destination / "keep.txt"), "caller-owned");
        const auto published = destination / "payload.txt";
        if (std::filesystem::exists(published)) {
            REQUIRE_EQ(publication_bytes(published), "complete verified payload");
        } else {
            // Removal before inventory may leave a valid empty tree; an observed stale entry must be rejected.
            REQUIRE_TRUE(removed || rejected);
        }
        flitzzip::DirectoryPublishTransaction retry(destination);
        std::ofstream(retry.staging_directory() / "payload.txt", std::ios::binary) << "retry complete";
        retry.publish(true);
        REQUIRE_EQ(publication_bytes(published), "retry complete");
        REQUIRE_EQ(publication_bytes(destination / "keep.txt"), "caller-owned");
    }
    REQUIRE_TRUE(stale_inventory_refusals > 0);
}

// Purpose: Retain caller files when a real Windows sharing owner prevents complete quarantine inventory.
// Inputs: A tiny staged child directory held exclusively, then released on the same live transaction.
// Outputs: Refuses incomplete inventory, retains staged bytes, then publishes exact files after owner release.
TEST_CASE(publication_state_shared_directory_inventory_refusal_recovers_same_transaction) {
    const auto root = test_temp_dir("publication-inventory-sharing");
    const auto destination = root / "out";
    std::filesystem::create_directory(destination);
    std::ofstream(destination / "keep.txt", std::ios::binary) << "caller-owned";
    std::filesystem::path staging;
    {
        flitzzip::DirectoryPublishTransaction transaction(destination);
        staging = transaction.staging_directory();
        const auto child = staging / "nested";
        std::filesystem::create_directory(child);
        std::ofstream(child / "payload.txt", std::ios::binary) << "verified inventory payload";
        const auto raw_blocker = CreateFileW(child.c_str(), FILE_LIST_DIRECTORY, 0, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        std::unique_ptr<void, decltype(&CloseHandle)> blocker(
            raw_blocker == INVALID_HANDLE_VALUE ? nullptr : raw_blocker, &CloseHandle);
        REQUIRE_TRUE(blocker != nullptr);
        // Calibrate actual filesystem refusal and separately establish that object metadata stays readable.
        std::error_code probe_error;
        const std::filesystem::directory_iterator probe(child, probe_error);
        REQUIRE_EQ(probe_error.value(), static_cast<int>(ERROR_SHARING_VIOLATION));
        probe_error.clear();
        REQUIRE_TRUE(std::filesystem::is_directory(std::filesystem::symlink_status(child, probe_error)));
        REQUIRE_TRUE(!probe_error);
        bool rejected = false;
        try {
            transaction.publish(false);
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string_view(error.what()).starts_with("cannot enumerate complete extraction quarantine:");
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(publication_bytes(destination / "keep.txt"), "caller-owned");
        REQUIRE_TRUE(!std::filesystem::exists(destination / "nested"));
        blocker.reset();
        REQUIRE_EQ(publication_bytes(child / "payload.txt"), "verified inventory payload");
        transaction.publish(false);
        REQUIRE_TRUE(!std::filesystem::exists(staging));
    }
    REQUIRE_TRUE(!std::filesystem::exists(staging));
    REQUIRE_EQ(publication_bytes(destination / "keep.txt"), "caller-owned");
    REQUIRE_EQ(publication_bytes(destination / "nested" / "payload.txt"), "verified inventory payload");
}

namespace {

// Purpose: Observe a real Windows file identity without retaining a sharing restriction during publication.
// Inputs: Owned ordinary fixture path. Outputs: Its volume and file identifier, or a failed test assertion.
std::array<DWORD, 3> publication_file_key(const std::filesystem::path& path) {
    flitzzip::windows_detail::HandleOwner handle(
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    REQUIRE_TRUE(handle != nullptr && handle.get() != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION information{};
    REQUIRE_TRUE(GetFileInformationByHandle(handle.get(), &information) != FALSE);
    return {information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow};
}

// Purpose: Check exact bytes and object identity independently of the publication implementation.
// Inputs: Existing path, previously observed native key and literal expected bytes. Outputs: Exact assertions.
void require_publication_snapshot(const std::filesystem::path& path, const std::array<DWORD, 3>& identity,
                                  std::string_view bytes) {
    REQUIRE_EQ(publication_file_key(path), identity);
    REQUIRE_EQ(publication_bytes(path), std::string(bytes));
}

}  // namespace

// Purpose: Keep a refused final rename retryable through the same transaction without disturbing another owner.
// Inputs: Tiny real files, an existing target held without delete sharing, and a separate live reservation.
// Outputs: Preserves target/stage identities and bytes, publishes once after release, and cleans only its own staging.
TEST_CASE(publication_state_final_rename_refusal_retains_live_owner_and_exact_payload) {
    const auto root = test_temp_dir("publication-final-rename-refusal");
    const auto target = root / "target.bin";
    const auto sibling_target = root / "sibling.bin";
    std::ofstream(target, std::ios::binary) << "caller-owned target";
    flitzzip::FilePublishTransaction publication(target);
    const auto stage = publication.staging_path();
    std::ofstream(stage, std::ios::binary) << "complete verified replacement";
    flitzzip::FilePublishTransaction sibling(sibling_target);
    const auto sibling_stage = sibling.staging_path();
    std::ofstream(sibling_stage, std::ios::binary) << "independently owned sibling";
    const auto target_key = publication_file_key(target);
    const auto stage_key = publication_file_key(stage);
    const auto sibling_key = publication_file_key(sibling_stage);
    REQUIRE_TRUE(target_key != stage_key);
    REQUIRE_TRUE(stage_key != sibling_key);
    flitzzip::windows_detail::HandleOwner blocker(CreateFileW(target.c_str(), GENERIC_READ,
                                                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE_TRUE(blocker != nullptr && blocker.get() != INVALID_HANDLE_VALUE);
    bool refused = false;
    try {
        publication.commit(true);
    } catch (const flitzzip::ArchiveError& error) {
        const std::string_view text(error.what());
        refused = text.starts_with("failed to publish verified output:") &&
                  (text.find("(Windows error 5)") != text.npos || text.find("(Windows error 32)") != text.npos);
    }
    REQUIRE_TRUE(refused);
    require_publication_snapshot(target, target_key, "caller-owned target");
    require_publication_snapshot(stage, stage_key, "complete verified replacement");
    require_publication_snapshot(sibling_stage, sibling_key, "independently owned sibling");
    REQUIRE_TRUE(!std::filesystem::exists(sibling_target));
    blocker.reset();
    publication.commit(true);
    require_publication_snapshot(target, stage_key, "complete verified replacement");
    REQUIRE_TRUE(!std::filesystem::exists(stage.parent_path()));
    require_publication_snapshot(sibling_stage, sibling_key, "independently owned sibling");
    bool repeated = false;
    try {
        publication.commit(true);
    } catch (const flitzzip::SecurityError& error) {
        repeated = std::string_view(error.what()) == "publication transaction is already committed";
    }
    REQUIRE_TRUE(repeated);
    require_publication_snapshot(target, stage_key, "complete verified replacement");
    sibling.commit(false);
    require_publication_snapshot(sibling_target, sibling_key, "independently owned sibling");
    REQUIRE_TRUE(!std::filesystem::exists(sibling_stage.parent_path()));
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 2);
}
