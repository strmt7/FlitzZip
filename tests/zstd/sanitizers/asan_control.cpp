#include <cstring>
#include <iostream>
#include <memory>
#include <new>

#ifndef __SANITIZE_ADDRESS__
#error The sanitizer qualification control must be instrumented.
#endif

// Purpose: Qualify the sanitizer using a valid heap access and one deliberately invalid test-only access.
// Inputs: Exactly --valid or --heap-overflow; the invalid access is confined to this standalone control process.
// Outputs: Returns zero for valid access; requires ASan to terminate and diagnose the invalid access.
int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "--valid") != 0 && std::strcmp(argv[1], "--heap-overflow") != 0)) {
        return 2;
    }
    const std::unique_ptr<unsigned char[]> storage(new (std::nothrow) unsigned char[16U]);
    if (!storage) {
        return 3;
    }
    volatile std::size_t offset = std::strcmp(argv[1], "--valid") == 0 ? 15U : 16U;
    storage.get()[offset] = 0x5CU;
    std::cout << static_cast<unsigned>(storage.get()[offset]) << '\n';
    return 0;
}
