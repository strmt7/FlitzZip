#include "core/integrity.hpp"

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace superzip {
namespace {

constexpr std::size_t kIntegrityBufferBytes = 1024U * 1024U;
constexpr auto kIntegrityTimeLimit = std::chrono::minutes(30);

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

class IntegrityHandle {
  public:
    explicit IntegrityHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    IntegrityHandle(const IntegrityHandle&) = delete;
    IntegrityHandle& operator=(const IntegrityHandle&) = delete;

    // Purpose: Transfer an opened no-follow integrity object without closing it.
    // Inputs: `other` relinquishes handle ownership.
    // Outputs: This object owns the handle and `other` becomes invalid.
    IntegrityHandle(IntegrityHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    // Purpose: Replace this handle with another move-owned integrity object.
    // Inputs: `other` relinquishes handle ownership.
    // Outputs: Closes any previous handle and leaves `other` invalid.
    IntegrityHandle& operator=(IntegrityHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    // Purpose: Close one no-follow file or directory handle.
    // Inputs: Uses the currently owned handle.
    // Outputs: Releases the OS object exactly once without throwing.
    ~IntegrityHandle() {
        close();
    }

    // Purpose: Expose the exact opened object to metadata and read APIs.
    // Inputs: None.
    // Outputs: Returns a borrowed valid handle.
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    // Purpose: Close the owned object if valid.
    // Inputs: Uses `handle_`.
    // Outputs: Invalidates this owner without throwing.
    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct IntegrityObjectState {
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    DWORD attributes = 0;
};

struct IntegrityObject {
    IntegrityHandle handle;
    IntegrityObjectState state;
};

// Purpose: Snapshot identity, size, timestamps, and type from one already-opened filesystem object.
// Inputs: `handle` was opened without following a reparse point.
// Outputs: Returns exact handle-bound metadata or throws for reparses, unsupported types, or API failure.
IntegrityObjectState query_integrity_object_state(HANDLE handle) {
    IntegrityObjectState state;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &state.id, sizeof(state.id)) == 0 ||
        GetFileInformationByHandleEx(handle, FileBasicInfo, &state.basic, sizeof(state.basic)) == 0 ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &state.standard, sizeof(state.standard)) == 0 ||
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0) {
        throw ArchiveError("cannot inspect integrity target through its open handle");
    }
    state.attributes = tag.FileAttributes;
    if ((state.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U || state.standard.DeletePending != FALSE ||
        state.standard.EndOfFile.QuadPart < 0) {
        throw SecurityError("SHA-256 integrity hashing rejects reparse or deleting objects");
    }
    return state;
}

// Purpose: Open and lock one exact file or directory without following reparses or permitting mutation/delete sharing.
// Inputs: `path` is an absolute or caller-selected existing filesystem path.
// Outputs: Returns an owned handle and initial identity snapshot, or throws without traversing an unsafe object.
IntegrityObject open_integrity_object(const std::filesystem::path& path) {
    const auto handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw ArchiveError("cannot open and lock integrity target: " + path.string());
    }
    IntegrityHandle owner(handle);
    return IntegrityObject{.handle = std::move(owner), .state = query_integrity_object_state(handle)};
}

// Purpose: Compare two snapshots from the same open integrity handle.
// Inputs: `before` and `after` bracket enumeration or payload hashing.
// Outputs: Returns true only when identity, size, write time, and attributes remained unchanged.
bool integrity_state_matches(const IntegrityObjectState& before, const IntegrityObjectState& after) {
    return before.id.VolumeSerialNumber == after.id.VolumeSerialNumber &&
           std::memcmp(before.id.FileId.Identifier, after.id.FileId.Identifier, sizeof(before.id.FileId.Identifier)) ==
               0 &&
           before.standard.EndOfFile.QuadPart == after.standard.EndOfFile.QuadPart &&
           before.basic.LastWriteTime.QuadPart == after.basic.LastWriteTime.QuadPart &&
           before.attributes == after.attributes;
}

#endif

#ifdef _WIN32

struct IntegrityBudget {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::uint64_t entries = 0;
    std::uint64_t path_bytes = 0;
    std::uint64_t content_bytes = 0;

    // Purpose: Charge one discovered tree entry and its retained path metadata.
    // Inputs: `path_size` is the UTF-8 relative key length.
    // Outputs: Updates cumulative counters or throws at entry, path, or elapsed-time limits.
    void charge_entry(std::size_t path_size) {
        if (++entries > kMaxArchiveEntries || path_size > kMaxArchivePathBytes ||
            path_bytes > kMaxArchivePathMetadataBytes - path_size) {
            throw ArchiveError("SHA-256 tree metadata exceeds SuperZip resource limits");
        }
        path_bytes += path_size;
        check_time();
    }

    // Purpose: Charge file bytes before feeding them to the integrity hash.
    // Inputs: `bytes` is the next successful handle-bound read length.
    // Outputs: Updates cumulative content work or throws above the global output budget.
    void charge_content(std::uint64_t bytes) {
        if (bytes > kMaxExtractedOutputBytes || content_bytes > kMaxExtractedOutputBytes - bytes) {
            throw ArchiveError("SHA-256 content work exceeds SuperZip resource limits");
        }
        content_bytes += bytes;
        check_time();
    }

    // Purpose: Enforce the cumulative wall-time bound for one hash operation.
    // Inputs: Uses the construction timestamp.
    // Outputs: Throws when hashing exceeds the fixed product time limit.
    void check_time() const {
        if (std::chrono::steady_clock::now() - started > kIntegrityTimeLimit) {
            throw ArchiveError("SHA-256 hashing exceeded the SuperZip time limit");
        }
    }
};

// Purpose: Convert a path under a directory root to a stable lexical UTF-8 slash-separated key.
// Inputs: `root` is the hashed directory root and `path` is a direct or nested descendant.
// Outputs: Returns a deterministic relative path without filesystem resolution or throws for escape/size violations.
std::string relative_integrity_key(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_normal().lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        throw ArchiveError("cannot compute relative integrity path: " + path.string());
    }
    for (const auto& component : relative) {
        if (component == L"..") {
            throw SecurityError("integrity path escaped the selected directory");
        }
    }
    const auto utf8 = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// Purpose: Stream one exact locked regular file handle into a SHA-256 context.
// Inputs: `hasher` receives bytes, `object` is a no-follow immutable handle, and result/budget receive counters.
// Outputs: Hashes the complete initial object and throws if identity, size, timestamp, read, or work limits change.
void hash_regular_file_contents(Sha256Hasher& hasher, IntegrityObject& object, IntegrityResult& result,
                                IntegrityBudget& budget) {
    if ((object.state.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        throw ArchiveError("hash target is not a regular file");
    }
    std::vector<std::byte> buffer(kIntegrityBufferBytes);
    std::uint64_t read_total = 0;
    while (true) {
        DWORD count = 0;
        if (ReadFile(object.handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) == 0) {
            throw ArchiveError("cannot read complete hash target through its locked handle");
        }
        if (count == 0U) {
            break;
        }
        budget.charge_content(count);
        hasher.update(std::span<const std::byte>(buffer.data(), count));
        read_total += count;
    }
    const auto after = query_integrity_object_state(object.handle.get());
    if (!integrity_state_matches(object.state, after) ||
        read_total != static_cast<std::uint64_t>(object.state.standard.EndOfFile.QuadPart)) {
        throw SecurityError("hash target changed while its integrity digest was computed");
    }
    result.bytes_hashed += read_total;
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

struct IntegrityChild {
    std::filesystem::path path;
    std::string relative_path;
};

// Purpose: Enumerate and sort direct children while the exact directory remains locked against replacement.
// Inputs: `root` and `directory` are normalized paths and `budget` receives cumulative entry/path work.
// Outputs: Returns deterministic child records or throws on enumeration, fanout, metadata, or time limits.
std::vector<IntegrityChild> enumerate_integrity_children(const std::filesystem::path& root,
                                                         const std::filesystem::path& directory,
                                                         IntegrityBudget& budget) {
    std::error_code error;
    std::vector<IntegrityChild> children;
    for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (children.size() >= kMaxArchiveEntries) {
            throw ArchiveError("SHA-256 directory fanout exceeds SuperZip resource limits");
        }
        auto relative = relative_integrity_key(root, iterator->path());
        budget.charge_entry(relative.size());
        children.push_back(IntegrityChild{.path = iterator->path(), .relative_path = std::move(relative)});
    }
    if (error) {
        throw ArchiveError("cannot enumerate complete integrity directory: " + error.message());
    }
    std::ranges::sort(children,
                      [](const auto& left, const auto& right) { return left.relative_path < right.relative_path; });
    return children;
}

struct IntegrityDirectoryFrame {
    std::filesystem::path path;
    IntegrityObject object;
    std::vector<IntegrityChild> children;
    std::size_t next_child = 0;
    std::uint32_t depth = 0;
};

// Purpose: Iteratively hash a directory tree while retaining one immutable no-follow handle per active depth.
// Inputs: `hasher`, normalized `root`, root `object`, `result`, and `budget` describe one bounded operation.
// Outputs: Mutates the tree digest/counters and rejects reparses, races, unsupported objects, or excess work.
void hash_directory_tree(Sha256Hasher& hasher, const std::filesystem::path& root, IntegrityObject root_object,
                         IntegrityResult& result, IntegrityBudget& budget) {
    std::vector<IntegrityDirectoryFrame> frames;
    auto root_children = enumerate_integrity_children(root, root, budget);
    frames.push_back(IntegrityDirectoryFrame{
        .path = root,
        .object = std::move(root_object),
        .children = std::move(root_children),
        .next_child = 0,
        .depth = 0,
    });
    while (!frames.empty()) {
        auto& frame = frames.back();
        if (frame.next_child >= frame.children.size()) {
            const auto after = query_integrity_object_state(frame.object.handle.get());
            if (!integrity_state_matches(frame.object.state, after)) {
                throw SecurityError("integrity directory changed while it was enumerated");
            }
            frames.pop_back();
            continue;
        }
        const auto child = frame.children[frame.next_child++];
        auto object = open_integrity_object(child.path);
        const bool directory = (object.state.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        if (directory) {
            const auto depth = frame.depth + 1U;
            if (depth > kMaxArchivePathComponents) {
                throw ArchiveError("SHA-256 directory depth exceeds SuperZip resource limits");
            }
            update_tree_record(hasher, "D", child.relative_path);
            ++result.directories_hashed;
            auto children = enumerate_integrity_children(root, child.path, budget);
            frames.push_back(IntegrityDirectoryFrame{
                .path = child.path,
                .object = std::move(object),
                .children = std::move(children),
                .next_child = 0,
                .depth = depth,
            });
        } else {
            const auto size = static_cast<std::uint64_t>(object.state.standard.EndOfFile.QuadPart);
            update_tree_record(hasher, "F", child.relative_path, size);
            hash_regular_file_contents(hasher, object, result, budget);
            ++result.files_hashed;
        }
    }
}

// Purpose: Hash one regular file with standard SHA-256 semantics.
// Inputs: `object` is an already-opened regular file locked against mutation and replacement.
// Outputs: Returns digest and counters; throws when the target cannot be read.
IntegrityResult hash_regular_file(IntegrityObject object) {
    Sha256Hasher hasher;
    IntegrityBudget budget;
    IntegrityResult result{.attempted = true, .algorithm = "SHA-256", .target = "file"};
    hash_regular_file_contents(hasher, object, result, budget);
    result.files_hashed = 1;
    result.hex_digest = hasher.finish_hex();
    return result;
}

// Purpose: Hash one directory tree with SuperZip's deterministic SHA-256 tree contract.
// Inputs: `path` is an existing directory.
// Outputs: Returns digest and counters; throws when any entry is unsafe, unreadable, or unsupported.
IntegrityResult hash_directory(const std::filesystem::path& path, IntegrityObject object) {
    Sha256Hasher hasher;
    IntegrityBudget budget;
    IntegrityResult result{.attempted = true, .algorithm = "SHA-256", .target = "directory"};
    update_text(hasher, "SUPERZIP-DIRECTORY-SHA256");
    update_text(hasher, std::string_view{"\0v1\0", 4});
    result.directories_hashed = 1;
    hash_directory_tree(hasher, path, std::move(object), result, budget);
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
#ifndef _WIN32
    throw ArchiveError("SHA-256 integrity hashing is currently implemented through Windows CNG");
#else
    auto object = open_integrity_object(path);
    if ((object.state.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        throw ArchiveError("hash target is not a regular file: " + path.string());
    }
    return hash_regular_file(std::move(object));
#endif
}

// Purpose: Optionally compute a standard file hash or deterministic directory-tree hash.
// Inputs: `path` is an existing regular file or directory and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns digest and file/tree counters, or throws `ArchiveError` for missing or unsupported targets.
IntegrityResult hash_path(const std::filesystem::path& path, IntegrityMode mode) {
    if (mode == IntegrityMode::Disabled) {
        return {};
    }
#ifndef _WIN32
    throw ArchiveError("SHA-256 integrity hashing is currently implemented through Windows CNG");
#else
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        throw ArchiveError("cannot resolve hash target: " + error.message());
    }
    auto object = open_integrity_object(absolute);
    if ((object.state.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return hash_directory(absolute, std::move(object));
    }
    return hash_regular_file(std::move(object));
#endif
}

}  // namespace superzip
