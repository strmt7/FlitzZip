#include "test_util.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32) && SUPERZIP_ENABLE_HIP
#include <windows.h>
#endif

namespace {

struct Test {
    std::string name;
    TestFunction fn;
};

// Purpose: Store and expose the process-wide test registry.
// Inputs: None.
// Outputs: Returns a mutable registry used by static test registration.
std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

}  // namespace

// Purpose: Add a test case to the local C++ test registry.
// Inputs: `name` is the test label and `fn` is a nonnull static test function; no captured owner is transferred.
// Outputs: Mutates the registry; used by `TEST_CASE` static initializers.
void register_test(std::string name, TestFunction fn) {
    registry().push_back(Test{std::move(name), fn});
}

// Purpose: Execute registered tests, optionally filtered by substring or exact name.
// Inputs: `argc`/`argv` may contain one filter; a leading '=' requests exact selection.
// Outputs: Returns zero for passing selected tests, one for failures, or two for invalid arguments/selections.
int main(int argc, char** argv) {
#if defined(_WIN32) && SUPERZIP_ENABLE_HIP
    if (GetModuleHandleA(SUPERZIP_HIP_RUNTIME_DLL_NAME) != nullptr ||
        GetModuleHandleA(SUPERZIP_HIP_KERNEL_DLL_NAME) != nullptr) {
        std::cerr << "HIP runtime/kernel registration occurred before explicit admission\n";
        return 1;
    }
#endif
    if (argc > 2) {
        std::cerr << "Expected at most one test-name filter\n";
        return 2;
    }
    const std::string filter = argc > 1 ? argv[1] : "";
    const bool exact = !filter.empty() && filter.front() == '=';
    const std::string requested = exact ? filter.substr(1) : filter;
    if (exact && requested.empty()) {
        std::cerr << "Exact test-name filter cannot be empty\n";
        return 2;
    }
    int failed = 0;
    std::size_t selected = 0;
    for (const auto& test : registry()) {
        if (exact ? test.name != requested : (!requested.empty() && test.name.find(requested) == std::string::npos)) {
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
