#include "test_util.hpp"

#include "app/settings_migration.hpp"
#include "app/settings_storage.hpp"
#include "app/main_window_support.hpp"

#include <fstream>
#include <array>
#include <sstream>
#include <barrier>
#include <thread>

namespace {

class SettingsWriteBuffer final : public std::stringbuf {
  public:
    // Purpose: Select an independent write or flush fault. Inputs: Failure flags. Outputs: A controlled stream sink.
    SettingsWriteBuffer(bool write_failure, bool flush_failure)
        : write_failure_(write_failure), flush_failure_(flush_failure) {}

    // Purpose: Model a short write before all settings bytes have reached storage.
    // Inputs: Supplied bytes and extent. Outputs: Fails immediately or delegates exact storage to stringbuf.
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        return write_failure_ ? 0 : std::stringbuf::xsputn(data, count);
    }

    // Purpose: Model failure while committing buffered settings bytes to the underlying stream.
    // Inputs: Buffered state. Outputs: Independent failure after a successful write, or successful synchronization.
    int sync() override {
        return flush_failure_ ? -1 : std::stringbuf::sync();
    }

  private:
    bool write_failure_;
    bool flush_failure_;
};

// Purpose: Read back a test settings file independently of the production JSON reader.
// Inputs: Owned fixture path. Outputs: Exact stored bytes; a missing file fails the test.
std::string read_settings_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

// Purpose: Share one settings identity across equivalent Windows path representations.
// Inputs: An extended-length path containing forward separators and its independently constructed native target.
// Outputs: Reads, writes, migration selection and locking all refer to the exact same file without host-wide settings.
TEST_CASE(settings_storage_normalizes_paths_at_native_boundaries) {
    const auto root = test_temp_dir("settings_path_alias");
    const auto canonical = root / "FlitzZip" / "settings-v4.json";
    std::filesystem::create_directories(canonical.parent_path());
    const std::filesystem::path alias(root.native() + L"/FlitzZip/settings-v4.json");
    flitzzip::app::AppSettings original;
    const auto saved = flitzzip::app::write_settings_file(alias, original, std::nullopt);
    REQUIRE_EQ(read_settings_fixture(canonical), saved);
    REQUIRE_EQ(*flitzzip::app::read_settings_document(alias), saved);
    REQUIRE_EQ(flitzzip::app::settings_read_path(alias), canonical);
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(alias), original));
    original.overwrite = !original.overwrite;
    {
        const flitzzip::app::SettingsWriterLock lock(canonical);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::app::write_settings_file(alias, original, saved));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(read_settings_fixture(canonical), saved);
    }
    static_cast<void>(flitzzip::app::write_settings_file(alias, original, saved));
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(canonical), original));
    REQUIRE_EQ(read_settings_fixture(canonical.wstring() + L".bak"), saved);
    std::filesystem::remove_all(root);
}

// Purpose: Keep frozen historical profiles executable as compatibility evidence, independent of the serializer.
// Inputs: Authored generation fixtures from source control. Outputs: Direct migration, exact preservation and stable
// intent.
TEST_CASE(settings_migration_reads_frozen_historical_profiles) {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures/settings";
    const auto root = test_temp_dir("settings_frozen_profiles");
    const auto current = root / "FlitzZip" / "settings-v4.json";
    const auto legacy = current.parent_path() / "settings.json";
    std::filesystem::create_directories(current.parent_path());
    for (const auto name : {"legacy-v1.json", "legacy-v2.json", "legacy-v3.json", "predecessor-v3.json"}) {
        const auto bytes = read_settings_fixture(fixtures / name);
        {
            std::ofstream file(legacy, std::ios::binary);
            file << bytes;
        }
        const auto parsed = flitzzip::app::read_settings_file(legacy);
        const std::string_view fixture_name(name);
        const auto expected_format = fixture_name == "legacy-v1.json"   ? L".tar.gz"
                                     : fixture_name == "legacy-v2.json" ? L".tar.zst"
                                     : fixture_name == "legacy-v3.json" ? L".flzip"
                                                                        : L".tbz2";
        REQUIRE_EQ(flitzzip::app::compression_format_extension(parsed.compression_format_index), expected_format);
        REQUIRE_EQ(parsed.compression_level_index, fixture_name == "legacy-v1.json"   ? 8
                                                   : fixture_name == "legacy-v2.json" ? 6
                                                   : fixture_name == "legacy-v3.json"
                                                       ? flitzzip::app::kNeutronCompressionLevelIndex
                                                       : 2);
        static_cast<void>(flitzzip::app::write_settings_file(current, parsed, std::nullopt));
        REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(current), parsed));
        REQUIRE_EQ(read_settings_fixture(legacy), bytes);
        std::filesystem::remove(current);
    }
    const auto future = current.parent_path() / "settings-v999.json";
    {
        std::ofstream file(future);
        file << "future profile must not be consumed";
    }
    REQUIRE_EQ(flitzzip::app::settings_read_path(current), legacy);
    std::filesystem::remove_all(root);
}

// Purpose: Exercise simultaneous writers without permitting a stale successful publication.
// Inputs: Two workers holding the same revision and independent drafts. Outputs: Exactly one complete commit wins.
TEST_CASE(settings_storage_serializes_racing_writers) {
    const auto root = test_temp_dir("settings_writer_race");
    const auto current = root / "settings-v4.json";
    const flitzzip::app::AppSettings baseline;
    const std::optional<std::string> revision = flitzzip::app::write_settings_file(current, baseline, std::nullopt);
    std::barrier ready(3);
    std::array<bool, 2> won{};
    std::array<std::exception_ptr, 2> failures{};
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            ready.arrive_and_wait();
            try {
                auto draft = baseline;
                draft.overwrite = index == 0;
                draft.show_operation_summary = index == 1;
                static_cast<void>(flitzzip::app::write_settings_file(current, draft, revision, &baseline));
                won[index] = true;
            } catch (const flitzzip::ArchiveError&) {
                // A busy lock and a stale revision both require an explicit reload.
            } catch (...) {
                failures[index] = std::current_exception();
            }
        });
    }
    ready.arrive_and_wait();
    for (auto& worker : workers)
        worker.join();
    for (const auto& failure : failures)
        if (failure)
            std::rethrow_exception(failure);
    REQUIRE_EQ(static_cast<int>(won[0]) + static_cast<int>(won[1]), 1);
    const auto saved = flitzzip::app::read_settings_file(current);
    REQUIRE_EQ(saved.overwrite, won[0]);
    REQUIRE_EQ(saved.show_operation_summary, won[1]);
    REQUIRE_EQ(read_settings_fixture(current.wstring() + L".bak"), *revision);
    std::filesystem::remove_all(root);
}

// Purpose: Require both writer admission and recovery publication before changing the current profile.
// Inputs: A held writer lock and then a real OS handle denying backup replacement. Outputs: No damage; retry succeeds.
TEST_CASE(settings_storage_recovers_after_lock_and_backup_failure) {
    const auto root = test_temp_dir("settings_backup_failure");
    const auto current = root / "settings-v4.json";
    const auto backup = std::filesystem::path(current.wstring() + L".bak");
    flitzzip::app::AppSettings original;
    const std::optional<std::string> revision = flitzzip::app::write_settings_file(current, original, std::nullopt);
    auto draft = original;
    draft.overwrite = !original.overwrite;
    {
        const flitzzip::app::SettingsWriterLock lock(current);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::app::write_settings_file(current, draft, revision));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    flitzzip::app::publish_settings_json(backup, "{\"retained\":true}");
    {
        const flitzzip::windows_detail::HandleOwner handle(
            CreateFileW(backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        REQUIRE_TRUE(handle.get() != INVALID_HANDLE_VALUE);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::app::write_settings_file(current, draft, revision));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(read_settings_fixture(current), *revision);
        REQUIRE_EQ(read_settings_fixture(backup), "{\"retained\":true}");
    }
    const auto saved = flitzzip::app::write_settings_file(current, draft, revision);
    REQUIRE_EQ(read_settings_fixture(current), saved);
    REQUIRE_EQ(read_settings_fixture(backup), *revision);
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(current), draft));
    std::filesystem::remove_all(root);
}

// Purpose: Preserve hardware-unavailable preferences until the user explicitly changes their coupled group.
// Inputs: A GPU-only saved choice, a normalized CPU UI baseline and unrelated then related edits.
// Outputs: Unrelated Apply keeps GPU intent and opaque data; choosing another format clears the whole intent.
TEST_CASE(settings_storage_preserves_normalized_gpu_intent) {
    const auto root = test_temp_dir("settings_gpu_intent");
    const auto current = root / "settings-v4.json";
    const std::string original = R"({"schema":"flitzzip.settings.v4","compressionFormat":".flzip",
        "compressionLevel":9,"neutronStarMode":true,"gpuRequired":true,"overwrite":false,
        "extension":{"precise":9007199254740993},"compressionLevelIndex":0,"verifyMetadataBeforeExtract":false})";
    flitzzip::app::publish_settings_json(current, original);
    auto normalized = flitzzip::app::read_settings_file(current);
    REQUIRE_TRUE(normalized.validate_before_publish);
    normalized.compression_level_index = 8;
    normalized.gpu_required = false;
    auto draft = normalized;
    draft.overwrite = true;
    const auto unrelated = flitzzip::app::write_settings_file(current, draft, original, &normalized);
    REQUIRE_TRUE(unrelated.find("9007199254740993") != std::string::npos);
    REQUIRE_TRUE(flitzzip::app::read_settings_file(current).gpu_required);
    REQUIRE_EQ(flitzzip::app::read_settings_file(current).compression_level_index,
               flitzzip::app::kNeutronCompressionLevelIndex);
    auto format_change = draft;
    format_change.compression_format_index =
        flitzzip::app::parse_settings_json(R"({"schema":"flitzzip.settings.v4","compressionFormat":".zip"})")
            .compression_format_index;
    static_cast<void>(flitzzip::app::write_settings_file(current, format_change, unrelated, &draft));
    const auto saved = flitzzip::app::read_settings_file(current);
    REQUIRE_TRUE(!saved.gpu_required);
    REQUIRE_EQ(saved.compression_level_index, 8);
    REQUIRE_EQ(flitzzip::app::compression_format_extension(saved.compression_format_index), L".zip");
    std::filesystem::remove_all(root);
}

// Purpose: Keep every supported historical profile directly migratable without intermediate application installs.
// Inputs: Frozen schema identifiers, old format rows, renamed fields and independent future extension data.
// Outputs: Correct semantic preferences in v4, unchanged legacy bytes and retained unknown values.
TEST_CASE(settings_migration_skips_releases_without_losing_values) {
    const auto root = test_temp_dir("settings_direct_upgrade");
    const auto profile = root / "FlitzZip";
    std::filesystem::create_directories(profile);
    const auto current = profile / "settings-v4.json";
    const auto legacy = profile / "settings.json";
    for (const auto schema : {"flitzzip.settings.v1", "flitzzip.settings.v2", "flitzzip.settings.v3",
                              "superzip.settings.v1", "superzip.settings.v3"}) {
        const bool v1 = std::string_view(schema).ends_with("v1");
        const std::string json = "{\"schema\":\"" + std::string(schema) +
                                 "\",\"compressionFormatIndex\":" + (v1 ? "3" : "8") +
                                 R"(,"compressionLevelIndex":4,"logLevelIndex":2,"logRetentionIndex":1,
            "verifyMetadataBeforeExtract":false,"x-new-feature":{"integer":9007199254740993}})";
        {
            std::ofstream file(legacy, std::ios::binary);
            file << json;
        }
        const auto old = flitzzip::app::read_settings_file(flitzzip::app::settings_read_path(current));
        const std::optional<std::string> absent;
        const auto saved = flitzzip::app::write_settings_file(current, old, absent);
        const auto loaded = flitzzip::app::read_settings_file(current);
        REQUIRE_TRUE(flitzzip::app::settings_equal(old, loaded));
        REQUIRE_EQ(flitzzip::app::compression_format_extension(loaded.compression_format_index),
                   v1 ? L".tar.gz" : L".tar.zst");
        REQUIRE_TRUE(saved.find("flitzzip.settings.v4") != std::string::npos);
        REQUIRE_TRUE(saved.find("9007199254740993") != std::string::npos);
        REQUIRE_EQ(read_settings_fixture(legacy), json);
        REQUIRE_TRUE(!loaded.validate_before_publish);
        REQUIRE_EQ(loaded.log_level_index, 2);
        REQUIRE_EQ(loaded.log_retention_index, 1);
        std::filesystem::remove(current);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject unknown/corrupt generations instead of rewriting them as current defaults.
// Inputs: Future, mistyped and malformed profile documents. Outputs: No replacement or backup damage.
TEST_CASE(settings_migration_rejects_unknown_generations_without_writes) {
    const auto root = test_temp_dir("settings_future_generation");
    const auto current = root / "settings-v4.json";
    const auto backup = std::filesystem::path(current.wstring() + L".bak");
    for (const auto text : {R"({"schema":"flitzzip.settings.v999","new":true})", R"({"schema":4})",
                            R"({"schema":"other.product.v4"})", "invalid JSON"}) {
        {
            std::ofstream file(current, std::ios::binary);
            file << text;
        }
        {
            std::ofstream file(backup, std::ios::binary);
            file << "retained recovery bytes";
        }
        bool rejected = false;
        try {
            static_cast<void>(
                flitzzip::app::write_settings_file(current, {}, flitzzip::app::read_settings_document(current)));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(read_settings_fixture(current), text);
        REQUIRE_EQ(read_settings_fixture(backup), "retained recovery bytes");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent stale windows from overwriting another instance's preferences and retain a valid recovery point.
// Inputs: Two edits against one captured revision. Outputs: First commit succeeds; stale Apply fails without damage.
TEST_CASE(settings_storage_rejects_stale_revisions_and_preserves_recovery) {
    const auto root = test_temp_dir("settings_concurrent_revision");
    const auto current = root / "settings-v4.json";
    flitzzip::app::AppSettings baseline;
    const auto initial = flitzzip::app::write_settings_file(current, baseline, std::nullopt);
    const std::optional<std::string> revision(initial);
    auto first = baseline;
    first.show_operation_summary = !baseline.show_operation_summary;
    const auto committed = flitzzip::app::write_settings_file(current, first, revision, &baseline);
    auto stale = baseline;
    stale.overwrite = !baseline.overwrite;
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::app::write_settings_file(current, stale, revision, &baseline));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_settings_fixture(current), committed);
    REQUIRE_EQ(read_settings_fixture(current.wstring() + L".bak"), initial);
    REQUIRE_TRUE(flitzzip::app::settings_equal(flitzzip::app::read_settings_file(current), first));
    std::filesystem::remove_all(root);
}

// Purpose: Distinguish buffered-write success from complete successful output before publication.
// Inputs: Independent short-write and flush failures, exact byte limit and invalid serialized sizes.
// Outputs: Both transport failures are rejected; valid output is complete and size violations write nothing.
TEST_CASE(settings_storage_requires_successful_write_and_flush) {
    constexpr std::string_view json = "{\"overwrite\":false}";
    for (const auto [write_failure, flush_failure] : {std::pair{true, false}, std::pair{false, true}}) {
        SettingsWriteBuffer buffer(write_failure, flush_failure);
        std::ostream output(&buffer);
        bool rejected = false;
        try {
            flitzzip::app::write_settings_json_stream(output, json);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(buffer.str(), write_failure ? std::string{} : std::string(json));
    }
    std::string exact = "{}";
    exact.resize(flitzzip::app::kMaxSettingsJsonBytes, ' ');
    std::ostringstream good;
    flitzzip::app::write_settings_json_stream(good, exact);
    REQUIRE_EQ(good.str(), exact);
    for (const auto& invalid : {std::string{}, exact + ' '}) {
        std::ostringstream output;
        bool rejected = false;
        try {
            flitzzip::app::write_settings_json_stream(output, invalid);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(output.str().empty());
    }
}

// Purpose: Preserve prior settings and unrelated files across successful replacement and failed saves.
// Inputs: Private settings directory, a preexisting legacy .tmp filename, and an oversized snapshot.
// Outputs: Byte-exact replacement, no shared temporary-file clobbering, and no abandoned staging directories.
TEST_CASE(settings_storage_preserves_prior_file_and_uses_private_staging) {
    const auto root = test_temp_dir("settings_storage");
    const auto target = root / "settings.json";
    const auto unrelated = root / "settings.json.tmp";
    {
        std::ofstream sentinel(unrelated, std::ios::binary);
        sentinel << "unrelated temporary-file owner";
    }
    constexpr std::string_view first = "{\"overwrite\":false,\"gpuRequired\":true}";
    constexpr std::string_view second = "{\"overwrite\":true,\"gpuRequired\":false}";
    flitzzip::app::publish_settings_json(target, first);
    REQUIRE_EQ(read_settings_fixture(target), first);
    flitzzip::app::publish_settings_json(target, second);
    REQUIRE_EQ(read_settings_fixture(target), second);
    bool rejected = false;
    try {
        flitzzip::app::publish_settings_json(target, std::string(flitzzip::app::kMaxSettingsJsonBytes + 1U, ' '));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_settings_fixture(target), second);
    REQUIRE_EQ(read_settings_fixture(unrelated), "unrelated temporary-file owner");
    for (const auto& item : std::filesystem::directory_iterator(root)) {
        REQUIRE_TRUE(item.path() == target || item.path() == unrelated);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep the previous settings file when the OS refuses replacement, without leaking staging state.
// Inputs: A real Windows handle denying delete sharing on an existing file. Outputs: Save fails and bytes survive.
TEST_CASE(settings_storage_rejects_locked_replacement_without_data_loss) {
    const auto root = test_temp_dir("settings_storage_locked");
    const auto target = root / "settings.json";
    flitzzip::app::publish_settings_json(target, "{\"overwrite\":false}");
    const auto handle = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
    bool rejected = false;
    try {
        flitzzip::app::publish_settings_json(target, "{\"overwrite\":true}");
    } catch (const std::exception&) {
        rejected = true;
    }
    CloseHandle(handle);
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_settings_fixture(target), "{\"overwrite\":false}");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
    std::filesystem::remove_all(root);
}

// Purpose: Preserve saved choices across the product rename without replacing current settings or smoke isolation.
// Inputs: An isolated predecessor/current directory pair and deliberately invalid current contents.
// Outputs: Fails if migration loses precedence, modifies predecessor bytes, or applies to a smoke filename.
TEST_CASE(settings_migration_preserves_precedence_and_smoke_isolation) {
    const auto root = test_temp_dir("settings_migration");
    const auto current = root / "FlitzZip" / "settings.json";
    const auto predecessor = root / "SuperZip" / "settings.json";
    REQUIRE_EQ(flitzzip::app::settings_read_path(current), current);
    std::filesystem::create_directories(predecessor.parent_path());
    {
        std::ofstream output(predecessor);
        output << "{\"requireGpu\":true}";
    }
    REQUIRE_EQ(flitzzip::app::settings_read_path(current), predecessor);
    const auto smoke = current.parent_path() / "gui-smoke-settings.json";
    REQUIRE_EQ(flitzzip::app::settings_read_path(smoke), smoke);
    std::filesystem::create_directories(current.parent_path());
    {
        std::ofstream output(current);
        output << "invalid current settings";
    }
    REQUIRE_EQ(flitzzip::app::settings_read_path(current), current);
    {
        std::ifstream input(predecessor);
        std::string unchanged;
        std::getline(input, unchanged);
        REQUIRE_EQ(unchanged, "{\"requireGpu\":true}");
    }
    std::filesystem::remove_all(root);
}
