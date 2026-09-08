#include "app/operation_destination.hpp"
#include "test_util.hpp"

TEST_CASE(operation_destination_uses_output_and_captured_preference_for_each_job_kind) {
    using superzip::OperationKind;
    using superzip::app::operation_destination_path;
    const auto directory = std::filesystem::path(u8"C:/output folder/\u03a9");
    const auto archive = directory / L"archive.suzip";
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
    superzip::app::OperationDestination completion;
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
    superzip::app::OperationDestination completion;
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
