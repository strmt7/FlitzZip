#include "test_util.hpp"

#include <exception>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && FLITZZIP_ENABLE_HIP
#include <windows.h>
#endif

namespace {

struct Test {
    std::string name;
    TestFunction fn;
};

enum class TestNameMatch { Substring, Exact, Prefix };

struct TestNameFilter {
    TestNameMatch mode = TestNameMatch::Substring;
    std::string_view requested;
};

// Purpose: Parse one explicit test selection without copying or changing legacy substring semantics.
// Inputs: A borrowed selector retained throughout matching; '=' and '^' introduce exact and prefix modes.
// Outputs: A borrowed filter, or invalid_argument for an empty explicitly anchored selection.
TestNameFilter parse_test_name_filter(std::string_view selector) {
    TestNameFilter filter{.requested = selector};
    if (selector.starts_with('=')) {
        filter.mode = TestNameMatch::Exact;
        filter.requested.remove_prefix(1U);
    } else if (selector.starts_with('^')) {
        filter.mode = TestNameMatch::Prefix;
        filter.requested.remove_prefix(1U);
    }
    if (filter.requested.empty() && filter.mode != TestNameMatch::Substring) {
        throw std::invalid_argument(filter.mode == TestNameMatch::Exact ? "Exact test-name filter cannot be empty"
                                                                        : "Prefix test-name filter cannot be empty");
    }
    return filter;
}

// Purpose: Match a registered name against one parsed selector with explicit boundary semantics.
// Inputs: Borrowed name and filter views whose owners remain live. Outputs: True only for the selected match mode.
bool matches_test_name(std::string_view name, TestNameFilter filter) {
    switch (filter.mode) {
    case TestNameMatch::Exact:
        return name == filter.requested;
    case TestNameMatch::Prefix:
        return name.starts_with(filter.requested);
    case TestNameMatch::Substring:
        return name.find(filter.requested) != std::string_view::npos;
    }
    return false;
}

// Purpose: Store and expose the process-wide test registry.
// Inputs: None.
// Outputs: Returns a mutable registry used by static test registration.
std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

}  // namespace

// Purpose: Prevent focused format selection from unintentionally invoking similarly named GPU/codec families.
// Inputs: Independent expected results for exact, anchored-prefix and retained legacy substring behavior.
// Outputs: Requires the specified boundary behavior, including lzip/flzip and tar/Neutron collisions.
TEST_CASE(test_name_filter_modes_preserve_selection_boundaries) {
    struct ExpectedMatch {
        std::string_view selector;
        std::string_view name;
        bool matches;
    };
    constexpr std::array<ExpectedMatch, 10> cases{{
        {"lzip_", "flzip_gpu_roundtrip", true},
        {"^lzip_", "flzip_gpu_roundtrip", false},
        {"^lzip_", "lzip_extracts_fixture", true},
        {"^tar_", "neutron_star_gpu_pipeline", false},
        {"tar_", "neutron_star_gpu_pipeline", true},
        {"=lzip_extract", "lzip_extract_else", false},
        {"=lzip_extract", "lzip_extract", true},
        {"", "any_registered_name", true},
        {"source_", "single_stream_source_session", true},
        {"^source_", "single_stream_source_session", false},
    }};
    for (const auto& expected : cases) {
        REQUIRE_EQ(matches_test_name(expected.name, parse_test_name_filter(expected.selector)), expected.matches);
    }
}

// Purpose: Reject empty anchored filters rather than running or silently selecting an unintended test cohort.
// Inputs: Explicit exact and prefix markers with no requested name. Outputs: The precise invalid-selection diagnostic.
TEST_CASE(test_name_filter_empty_anchored_requests_are_rejected) {
    for (const std::string_view selector : {"=", "^"}) {
        bool rejected = false;
        try {
            (void)parse_test_name_filter(selector);
        } catch (const std::invalid_argument& error) {
            const auto expected =
                selector == "=" ? "Exact test-name filter cannot be empty" : "Prefix test-name filter cannot be empty";
            REQUIRE_EQ(std::string_view(error.what()), std::string_view(expected));
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Add a test case to the local C++ test registry.
// Inputs: `name` is the test label and `fn` is a nonnull static test function; no captured owner is transferred.
// Outputs: Mutates the registry; used by `TEST_CASE` static initializers.
void register_test(std::string name, TestFunction fn) {
    registry().push_back(Test{std::move(name), fn});
}

// Purpose: Execute registered tests, optionally filtered by substring, exact name or anchored prefix.
// Inputs: `argc`/`argv` may contain one filter; leading '=' requests exact selection, '^' requests prefix selection.
// Outputs: Returns zero for passing selected tests, one for failures, or two for invalid arguments/selections.
int main(int argc, char** argv) {
#if defined(_WIN32) && FLITZZIP_ENABLE_HIP
    if (GetModuleHandleA(FLITZZIP_HIP_RUNTIME_DLL_NAME) != nullptr ||
        GetModuleHandleA(FLITZZIP_HIP_KERNEL_DLL_NAME) != nullptr) {
        std::cerr << "HIP runtime/kernel registration occurred before explicit admission\n";
        return 1;
    }
#endif
    if (argc > 2) {
        std::cerr << "Expected at most one test-name filter\n";
        return 2;
    }
    TestNameFilter filter;
    try {
        filter = parse_test_name_filter(argc > 1 ? std::string_view(argv[1]) : std::string_view{});
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\n";
        return 2;
    }
    int failed = 0;
    std::size_t selected = 0;
    for (const auto& test : registry()) {
        if (!matches_test_name(test.name, filter)) {
            continue;
        }
        ++selected;
        try {
            std::cout << "[RUN ] " << test.name << "\n" << std::flush;
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    if (selected == 0) {
        std::cerr << "No tests matched the requested filter\n";
        return 2;
    }
    std::cout << selected << " tests, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
