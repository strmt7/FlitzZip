#include "test_util.hpp"

#include "app/settings_migration.hpp"
#include "app/settings_storage.hpp"

#include <fstream>
#include <sstream>

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
