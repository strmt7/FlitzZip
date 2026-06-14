#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {

// Purpose: Create and cache a stable destination root for path-safety fuzzing.
// Inputs: None; uses the platform temporary directory.
// Outputs: Returns an existing directory path or throws through `std::filesystem`.
const std::filesystem::path& fuzz_root() {
    static const std::filesystem::path root = [] {
        auto path = std::filesystem::temp_directory_path() / "superzip-path-safety-fuzz-root";
        std::filesystem::create_directories(path);
        return path;
    }();
    return root;
}

}  // namespace

// Purpose: Feed arbitrary archive-entry names into SuperZip path canonicalization.
// Inputs: `data` and `size` are libFuzzer-owned bytes for one fuzz iteration.
// Outputs: Returns 0 after accepted normalization or expected security rejection; sanitizer findings crash the process.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size > 4096U) {
        return 0;
    }

    const std::string entry(reinterpret_cast<const char*>(data), size);
    try {
        (void)superzip::safe_join_archive_path(fuzz_root(), entry);
    } catch (const superzip::Error&) {
    } catch (const std::filesystem::filesystem_error&) {
    }
    return 0;
}
