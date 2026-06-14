#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using TestFn = std::function<void()>;

struct Test {
    std::string name;
    TestFn fn;
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
// Inputs: `name` is the test label and `fn` is the callable test body.
// Outputs: Mutates the registry; used by `TEST_CASE` static initializers.
void register_test(std::string name, TestFn fn) {
    registry().push_back(Test{std::move(name), std::move(fn)});
}

// Purpose: Execute registered tests, optionally filtered by substring.
// Inputs: `argc`/`argv` may contain one filter string.
// Outputs: Returns zero when all selected tests pass and nonzero on failure.
int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int failed = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
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
    std::cout << registry().size() << " tests, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
