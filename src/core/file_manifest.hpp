#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace superzip {

struct SourceFileIdentity {
    std::uint64_t volume_serial = 0;
    std::array<std::uint8_t, 16> file_id{};
    std::uint64_t size = 0;
    std::int64_t last_write_time = 0;
    bool available = false;
};

struct ManifestEntry {
    std::filesystem::path source_path;
    std::string archive_path;
    bool directory = false;
    std::uint64_t size = 0;
    SourceFileIdentity identity;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t file_count = 0;
    std::uint64_t path_metadata_bytes = 0;
};

class ManifestSourceLockState;

class ManifestSourceLock {
  public:
    ManifestSourceLock() = default;
    explicit ManifestSourceLock(std::shared_ptr<ManifestSourceLockState> state) : state_(std::move(state)) {}

  private:
    std::shared_ptr<ManifestSourceLockState> state_;
};

class PinnedSourceFile {
  public:
    PinnedSourceFile(const PinnedSourceFile&) = delete;
    PinnedSourceFile& operator=(const PinnedSourceFile&) = delete;
    PinnedSourceFile(PinnedSourceFile&&) noexcept = default;
    PinnedSourceFile& operator=(PinnedSourceFile&&) noexcept = default;

    // Purpose: Return the normalized path whose object and parent chain remain pinned.
    // Inputs: None.
    // Outputs: Returns a stable path reference without transferring the underlying lock.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    // Purpose: Return the file size captured from the pinned object handle.
    // Inputs: None.
    // Outputs: Returns the exact byte length verified when the source lock was acquired.
    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

  private:
    friend PinnedSourceFile pin_source_file(const std::filesystem::path& path);

    // Purpose: Bind a normalized path and size to an already-acquired source lock.
    // Inputs: `path`, `size`, and `lock` describe the same verified regular-file object.
    // Outputs: Constructs an RAII source session that releases the lock on destruction.
    PinnedSourceFile(std::filesystem::path path, std::uint64_t size, ManifestSourceLock lock)
        : path_(std::move(path)), size_(size), lock_(std::move(lock)) {}

    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    ManifestSourceLock lock_;
};

// Purpose: Convert filesystem sources into deterministic archive entries.
// Inputs: `sources` are user-selected files or directories; symlinks and unsupported file types are rejected.
// Outputs: Returns sorted manifest metadata with normalized archive names; throws `ArchiveError` or `SecurityError` for
// invalid sources.
Manifest build_manifest(const std::vector<std::filesystem::path>& sources);

// Purpose: Pin and verify a regular source file before a format writer reopens its pathname.
// Inputs: `entry` is a non-directory manifest entry with captured object identity.
// Outputs: Returns an RAII lock that prevents source/parent replacement and mutation until destroyed, or throws.
ManifestSourceLock lock_manifest_source(const ManifestEntry& entry);

// Purpose: Pin and verify one regular source file for a complete multi-stage operation.
// Inputs: `path` identifies an existing non-reparse regular file.
// Outputs: Returns its normalized path, exact size, and an RAII lock that prevents mutation or replacement.
PinnedSourceFile pin_source_file(const std::filesystem::path& path);

}  // namespace superzip
