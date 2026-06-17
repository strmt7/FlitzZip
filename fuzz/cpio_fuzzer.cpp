#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMaxCpioFuzzInputBytes = 1U * 1024U * 1024U;

// Purpose: Return a process-specific temporary directory for CPIO fuzz iterations.
// Inputs: None; uses the platform temporary directory and process id.
// Outputs: Returns an existing directory path or throws through `std::filesystem`.
const std::filesystem::path& fuzz_root() {
    static const std::filesystem::path root = [] {
#ifdef _WIN32
        const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#else
        const auto process_id = static_cast<unsigned long long>(getpid());
#endif
        auto path = std::filesystem::temp_directory_path() / ("superzip-cpio-fuzz-" + std::to_string(process_id));
        std::filesystem::create_directories(path);
        return path;
    }();
    return root;
}

// Purpose: Write one fuzz input to a temporary CPIO candidate file.
// Inputs: `data`/`size` are libFuzzer-owned bytes and `path` is the candidate file.
// Outputs: Creates or replaces `path`; throws only for host I/O failures.
void write_fuzz_input(const std::uint8_t* data, std::size_t size, const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

}  // namespace

// Purpose: Feed arbitrary bytes into the CPIO metadata parser and extraction validator.
// Inputs: `data` and `size` are libFuzzer-owned bytes for one fuzz iteration.
// Outputs: Returns 0 after accepted extraction or expected parser/security rejection; sanitizer findings crash the process.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size > kMaxCpioFuzzInputBytes) {
        return 0;
    }

    static std::atomic<unsigned long long> counter{0};
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    const auto archive = fuzz_root() / ("candidate-" + std::to_string(id) + ".cpio");
    const auto output = fuzz_root() / ("out-" + std::to_string(id));

    try {
        write_fuzz_input(data, size, archive);
        (void)superzip::extract_cpio(archive, output, true);
    } catch (const superzip::Error&) {
    } catch (const std::filesystem::filesystem_error&) {
    }

    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    std::filesystem::remove(archive, ignored);
    return 0;
}
