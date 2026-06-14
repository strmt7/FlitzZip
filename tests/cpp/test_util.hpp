#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <windows.h>

void register_test(std::string name, std::function<void()> fn);

#define TEST_CASE(name) \
    static void name(); \
    namespace { \
    struct name##_registrar { \
        name##_registrar() { register_test(#name, name); } \
    } name##_registrar_instance; \
    } \
    static void name()

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error(std::string("require failed: ") + #expr); \
        } \
    } while (false)

#define REQUIRE_EQ(lhs, rhs) \
    do { \
        const auto lhs_value = (lhs); \
        const auto rhs_value = (rhs); \
        if (!(lhs_value == rhs_value)) { \
            throw std::runtime_error(std::string("require equal failed: ") + #lhs + " != " + #rhs); \
        } \
    } while (false)

// Purpose: Create a clean per-process temporary directory for one test.
// Inputs: `name` is a stable test-specific label.
// Outputs: Removes any prior generated directory, recreates it, and returns the path.
inline std::filesystem::path test_temp_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("superzip-test-" + name + "-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
