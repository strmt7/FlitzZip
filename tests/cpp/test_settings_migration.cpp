#include "test_util.hpp"

#include "app/settings_migration.hpp"

#include <fstream>

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
