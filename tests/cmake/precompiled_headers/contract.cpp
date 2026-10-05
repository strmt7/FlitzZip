#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

extern "C" unsigned int fixture_c_value(void);
static_assert(sizeof(void*) == 8U);
static_assert(sizeof(TCHAR) == EXPECTED_CHARACTER_BYTES);
#ifdef min
#error Windows min macro must remain disabled
#endif

// Purpose: Verify explicit headers and target-local character/macro policies with and without PCH.
// Inputs: None. Outputs: Returns zero only for the same bounded C/C++ values in every build strategy.
int main() {
    const std::array<unsigned int, 3> values{1U, 2U, fixture_c_value()};
    const std::span<const unsigned int> view(values);
    const std::vector<unsigned int> copy(view.begin(), view.end());
    const std::filesystem::path path("contract");
    const std::string text = path.string();
    return copy == std::vector<unsigned int>{1U, 2U, 3U} && text == "contract" ? 0 : 1;
}
