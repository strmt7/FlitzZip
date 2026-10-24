#include "core/file_publish.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
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
