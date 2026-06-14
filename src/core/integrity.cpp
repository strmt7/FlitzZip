#include "core/integrity.hpp"

#include "core/result.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace superzip {

IntegrityResult hash_file(const std::filesystem::path& path, IntegrityMode mode) {
    if (mode == IntegrityMode::Disabled) {
        return {};
    }
    if (!std::filesystem::exists(path)) {
        throw ArchiveError("hash target does not exist: " + path.string());
    }
#ifndef _WIN32
    throw ArchiveError("SHA-256 integrity hashing is currently implemented through Windows CNG");
#else
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD data_len = 0;
    std::vector<UCHAR> object;
    std::array<UCHAR, 32> digest{};

    auto cleanup = [&]() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    };

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        cleanup();
        throw ArchiveError("cannot open Windows SHA-256 provider");
    }
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_len),
            sizeof(object_len),
            &data_len,
            0) != 0) {
        cleanup();
        throw ArchiveError("cannot query Windows SHA-256 object length");
    }
    object.resize(object_len);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_len, nullptr, 0, 0) != 0) {
        cleanup();
        throw ArchiveError("cannot create Windows SHA-256 hash");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        cleanup();
        throw ArchiveError("cannot open hash target: " + path.string());
    }
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got > 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) != 0) {
                cleanup();
                throw ArchiveError("Windows SHA-256 update failed");
            }
        }
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        cleanup();
        throw ArchiveError("Windows SHA-256 finalize failed");
    }
    cleanup();

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        hex << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return IntegrityResult{
        .attempted = true,
        .algorithm = "SHA-256",
        .hex_digest = hex.str(),
    };
#endif
}

}  // namespace superzip
