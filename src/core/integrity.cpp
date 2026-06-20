#include "core/integrity.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace superzip {
namespace {

constexpr std::size_t kIntegrityBufferBytes = 1024U * 1024U;

#ifdef _WIN32

class Sha256Hasher {
  public:
    // Purpose: Open one Windows CNG SHA-256 hash context.
    // Inputs: None.
    // Outputs: Owns the provider, hash handle, and object buffer until destruction.
    Sha256Hasher() {
        try {
            if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
                throw ArchiveError("cannot open Windows SHA-256 provider");
            }
            DWORD object_len = 0;
            DWORD data_len = 0;
            if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len),
                                  sizeof(object_len), &data_len, 0) != 0) {
                throw ArchiveError("cannot query Windows SHA-256 object length");
            }
            object_.resize(object_len);
            if (BCryptCreateHash(algorithm_, &hash_, object_.data(), object_len, nullptr, 0, 0) != 0) {
                throw ArchiveError("cannot create Windows SHA-256 hash");
            }
        } catch (...) {
            close_handles();
            throw;
        }
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    // Purpose: Release CNG hash resources.
    // Inputs: None.
    // Outputs: Closes handles if they were opened.
    ~Sha256Hasher() {
        close_handles();
    }

    // Purpose: Close any opened CNG handles.
    // Inputs: None.
    // Outputs: Releases hash and provider handles exactly once.
    void close_handles() noexcept {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
        }
    }

    // Purpose: Add raw bytes to the SHA-256 stream.
    // Inputs: `bytes` is the next contiguous payload range.
    // Outputs: Mutates the CNG hash state or throws when CNG rejects the update.
    void update(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            return;
        }
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                           static_cast<ULONG>(bytes.size()), 0) != 0) {
            throw ArchiveError("Windows SHA-256 update failed");
        }
    }

    // Purpose: Finalize the SHA-256 stream as lowercase hexadecimal text.
    // Inputs: None.
    // Outputs: Returns the 64-character digest or throws when CNG finalization fails.
    [[nodiscard]] std::string finish_hex() {
        std::array<UCHAR, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
            throw ArchiveError("Windows SHA-256 finalize failed");
        }
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (const auto byte : digest) {
            hex << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return hex.str();
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
};

// Purpose: Feed UTF-8 metadata text into a SHA-256 stream.
// Inputs: `hasher` is the mutable hash context and `text` is already UTF-8 compatible metadata.
// Outputs: Mutates the hash state.
void update_text(Sha256Hasher& hasher, std::string_view text) {
    hasher.update(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// Purpose: Return whether a Windows path is a reparse point.
// Inputs: `path` is an existing filesystem path.
// Outputs: Returns true for symlinks, junctions, and other reparse-point entries; throws when attributes cannot be
// read.
bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw ArchiveError("cannot inspect integrity target attributes: " + path.string());
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

#endif

// Purpose: Validate that a hash target exists before hashing is enabled.
// Inputs: `path` is supplied by the caller.
// Outputs: Throws `ArchiveError` when the target is missing or cannot be inspected.
void require_existing_hash_target(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec || !exists) {
        throw ArchiveError("hash target does not exist: " + path.string());
    }
}

#ifdef _WIN32

// Purpose: Convert a path under a directory root to a stable UTF-8 slash-separated key.
// Inputs: `root` is the hashed directory root and `path` is an entry below it.
// Outputs: Returns a deterministic relative path string or throws when the path cannot be relativized.
std::string relative_integrity_key(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, root, ec);
    if (ec || relative.empty()) {
        throw ArchiveError("cannot compute relative integrity path: " + path.string());
    }
    const auto utf8 = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// Purpose: Stream one file into a SHA-256 context.
// Inputs: `hasher` receives bytes, `path` is an existing regular file, and `result` receives counters.
// Outputs: Mutates the hash state and result counters; throws on read failures.
void hash_regular_file_contents(Sha256Hasher& hasher, const std::filesystem::path& path, IntegrityResult& result) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open hash target: " + path.string());
    }
    std::vector<char> buffer(kIntegrityBufferBytes);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got > 0) {
            hasher.update(std::as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(got))));
            result.bytes_hashed += static_cast<std::uint64_t>(got);
        }
    }
    if (input.bad()) {
        throw ArchiveError("cannot read complete hash target: " + path.string());
    }
}

// Purpose: Add one unambiguous tree metadata record to the directory hash.
// Inputs: `hasher`, `record_type`, `relative_path`, and optional `size` describe one directory-tree entry.
// Outputs: Mutates the hash state without reading file payload bytes.
void update_tree_record(Sha256Hasher& hasher, std::string_view record_type, std::string_view relative_path,
                        std::uint64_t size = 0) {
    update_text(hasher, record_type);
    update_text(hasher, std::string_view{"\0", 1});
    update_text(hasher, relative_path);
    update_text(hasher, std::string_view{"\0", 1});
    update_text(hasher, std::to_string(size));
    update_text(hasher, std::string_view{"\0", 1});
}

// Purpose: Return directory entries sorted by stable relative path.
// Inputs: `root` is the tree root and `directory` is the current directory.
// Outputs: Returns direct children only; throws instead of silently skipping unreadable entries.
std::vector<std::filesystem::path> sorted_directory_entries(const std::filesystem::path& root,
                                                            const std::filesystem::path& directory) {
    std::error_code ec;
    std::vector<std::filesystem::path> entries;
    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) {
        throw ArchiveError("cannot enumerate integrity directory: " + directory.string());
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        entries.push_back(iterator->path());
        iterator.increment(ec);
        if (ec) {
            throw ArchiveError("cannot enumerate complete integrity directory: " + directory.string());
        }
    }
    std::ranges::sort(entries, [&root](const auto& left, const auto& right) {
        return relative_integrity_key(root, left) < relative_integrity_key(root, right);
    });
    return entries;
}

// Purpose: Recursively hash a directory tree with deterministic entry ordering.
// Inputs: `hasher` is the mutable context, `root` is the tree root, `directory` is the current directory, and `result`
// tracks counts. Outputs: Mutates the hash state; rejects reparse points and unsupported entry types.
void hash_directory_tree(Sha256Hasher& hasher, const std::filesystem::path& root,
                         const std::filesystem::path& directory, IntegrityResult& result) {
    for (const auto& entry : sorted_directory_entries(root, directory)) {
        if (is_reparse_point(entry)) {
            throw ArchiveError("SHA-256 directory hashing rejects reparse points: " + entry.string());
        }
        std::error_code ec;
        const auto status = std::filesystem::status(entry, ec);
        if (ec) {
            throw ArchiveError("cannot inspect integrity directory entry: " + entry.string());
        }
        const auto relative = relative_integrity_key(root, entry);
        if (std::filesystem::is_directory(status)) {
            update_tree_record(hasher, "D", relative);
            ++result.directories_hashed;
            hash_directory_tree(hasher, root, entry, result);
        } else if (std::filesystem::is_regular_file(status)) {
            const auto size = std::filesystem::file_size(entry, ec);
            if (ec) {
                throw ArchiveError("cannot inspect integrity file size: " + entry.string());
            }
            update_tree_record(hasher, "F", relative, static_cast<std::uint64_t>(size));
            hash_regular_file_contents(hasher, entry, result);
            ++result.files_hashed;
        } else {
            throw ArchiveError("SHA-256 directory hashing rejects unsupported entry type: " + entry.string());
        }
    }
}

// Purpose: Hash one regular file with standard SHA-256 semantics.
// Inputs: `path` is an existing regular file.
// Outputs: Returns digest and counters; throws when the target cannot be read.
IntegrityResult hash_regular_file(const std::filesystem::path& path) {
    Sha256Hasher hasher;
    IntegrityResult result{.attempted = true, .algorithm = "SHA-256", .target = "file"};
    hash_regular_file_contents(hasher, path, result);
    result.files_hashed = 1;
    result.hex_digest = hasher.finish_hex();
    return result;
}

// Purpose: Hash one directory tree with SuperZip's deterministic SHA-256 tree contract.
// Inputs: `path` is an existing directory.
// Outputs: Returns digest and counters; throws when any entry is unsafe, unreadable, or unsupported.
IntegrityResult hash_directory(const std::filesystem::path& path) {
    if (is_reparse_point(path)) {
        throw ArchiveError("SHA-256 directory hashing rejects reparse points: " + path.string());
    }
    Sha256Hasher hasher;
    IntegrityResult result{.attempted = true, .algorithm = "SHA-256", .target = "directory"};
    update_text(hasher, "SUPERZIP-DIRECTORY-SHA256");
    update_text(hasher, std::string_view{"\0v1\0", 4});
    result.directories_hashed = 1;
    hash_directory_tree(hasher, path, path, result);
    result.hex_digest = hasher.finish_hex();
    return result;
}

#endif

}  // namespace

// Purpose: Optionally compute the standard SHA-256 digest for one regular file.
// Inputs: `path` is an existing regular file and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns digest and counters, or throws `ArchiveError` when the enabled target cannot be hashed.
IntegrityResult hash_file(const std::filesystem::path& path, IntegrityMode mode) {
    if (mode == IntegrityMode::Disabled) {
        return {};
    }
    require_existing_hash_target(path);
#ifndef _WIN32
    throw ArchiveError("SHA-256 integrity hashing is currently implemented through Windows CNG");
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw ArchiveError("hash target is not a regular file: " + path.string());
    }
    return hash_regular_file(path);
#endif
}

// Purpose: Optionally compute a standard file hash or deterministic directory-tree hash.
// Inputs: `path` is an existing regular file or directory and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns digest and file/tree counters, or throws `ArchiveError` for missing or unsupported targets.
IntegrityResult hash_path(const std::filesystem::path& path, IntegrityMode mode) {
    if (mode == IntegrityMode::Disabled) {
        return {};
    }
    require_existing_hash_target(path);
#ifndef _WIN32
    throw ArchiveError("SHA-256 integrity hashing is currently implemented through Windows CNG");
#else
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        throw ArchiveError("cannot inspect hash target: " + path.string());
    }
    if (std::filesystem::is_regular_file(status)) {
        return hash_regular_file(path);
    }
    if (std::filesystem::is_directory(status)) {
        return hash_directory(path);
    }
    throw ArchiveError("hash target is not a regular file or directory: " + path.string());
#endif
}

}  // namespace superzip
