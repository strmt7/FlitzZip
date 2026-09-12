#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace superzip {

class FilePublishReservationState;

struct ReservedFilePublishTarget {
    std::filesystem::path directory;
    std::filesystem::path file;
    std::shared_ptr<FilePublishReservationState> state;
};

// Purpose: Create and validate a directory chain without traversing Windows reparse points.
// Inputs: `directory` is the absolute or relative directory path to create and validate.
// Outputs: Creates missing directories or throws while the chain cannot be proven reparse-free.
void create_verified_directories(const std::filesystem::path& directory);

// Purpose: Reserve a private temporary file target beside a final output file.
// Inputs: `target` is the final file path that will later receive verified bytes.
// Outputs: Returns normalized native I/O paths (extended-length on Windows), or throws on reservation failure.
ReservedFilePublishTarget reserve_file_publish_target(const std::filesystem::path& target);

// Purpose: Remove a reserved private publication tree after success or failure.
// Inputs: `temporary` is the reserved temporary directory and payload path.
// Outputs: Releases reservation locks and best-effort removes only the state-bound private tree.
void cleanup_file_publish_target(const ReservedFilePublishTarget& temporary);

// Purpose: Publish a fully verified temporary file into its final path.
// Inputs: `temporary` owns the verified payload and pinned parent, `target` is its reserved final path, and `overwrite`
// controls replacement. Outputs: Atomically renames the exact payload handle relative to the pinned parent, or throws
// without deleting the caller-owned temporary tree.
void commit_verified_file(const ReservedFilePublishTarget& temporary, const std::filesystem::path& target,
                          bool overwrite);

class FilePublishTransaction {
  public:
    // Purpose: Reserve a private staging file and pin the final target parent for one write transaction.
    // Inputs: `target` is the final output file path.
    // Outputs: Constructs an active transaction or throws before caller writes begin.
    explicit FilePublishTransaction(const std::filesystem::path& target);
    FilePublishTransaction(const FilePublishTransaction&) = delete;
    FilePublishTransaction& operator=(const FilePublishTransaction&) = delete;
    ~FilePublishTransaction();

    // Purpose: Return the private path to which the caller must write complete verified bytes.
    // Inputs: None.
    // Outputs: Returns a stable path valid until commit or destruction.
    const std::filesystem::path& staging_path() const noexcept;

    // Purpose: Atomically publish the staged file to its state-bound final target.
    // Inputs: `overwrite` controls whether an existing final file may be replaced.
    // Outputs: Commits and cleans staging state, or throws while retaining destructor cleanup.
    void commit(bool overwrite);

  private:
    std::filesystem::path target_;
    ReservedFilePublishTarget temporary_;
    bool committed_ = false;
};

class DirectoryPublishTransaction {
  public:
    // Purpose: Reserve a private extraction quarantine beneath a verified destination root.
    // Inputs: `destination` is the final directory into which clean files will be merged.
    // Outputs: Constructs an active transaction with a protected staging tree or throws before extraction starts.
    explicit DirectoryPublishTransaction(const std::filesystem::path& destination);
    DirectoryPublishTransaction(const DirectoryPublishTransaction&) = delete;
    DirectoryPublishTransaction& operator=(const DirectoryPublishTransaction&) = delete;
    ~DirectoryPublishTransaction();

    // Purpose: Return the private directory that archive adapters may populate.
    // Inputs: None.
    // Outputs: Returns a stable protected native I/O path valid until publish or destruction. Child paths must use
    // native separators; archive members must still go through `safe_join_archive_path` before I/O.
    [[nodiscard]] const std::filesystem::path& staging_directory() const noexcept;

    // Purpose: Merge every verified staged directory and file into the final destination.
    // Inputs: `overwrite` controls replacement; optional `checkpoint` may throw to cancel bounded publication work.
    // Outputs: Publishes ordinary non-reparse files and removes quarantine. Failure can leave already-published files;
    // this is per-file atomic publication, not an atomic directory merge.
    void publish(bool overwrite, const std::function<void()>& checkpoint = {});

  private:
    std::filesystem::path destination_;
    ReservedFilePublishTarget quarantine_;
    bool published_ = false;
};

struct ExtractionPublicationOptions {
    std::filesystem::path destination;
    bool overwrite = false;
    bool validate_before_publish = false;
};

// Purpose: Run one adapter extraction, optionally withholding final files until all decoding and inspection succeed.
// Inputs: `options` selects destination and publication policy; `extract` validates and writes into the supplied path
// using the supplied overwrite policy. Optional `inspect` examines fully extracted private files; `checkpoint` may
// throw to cancel before extraction or during publication. Callbacks run synchronously and are never retained.
// Outputs: Calls `extract` once, stages when validation or inspection is requested, and removes private files on
// failure. Final publication is atomic per file, not per archive; disabling staging never disables the adapter's
// required checks.
void extract_with_publication(const ExtractionPublicationOptions& options,
                              const std::function<void(const std::filesystem::path&, bool)>& extract,
                              const std::function<void(const std::filesystem::path&)>& inspect = {},
                              const std::function<void()>& checkpoint = {});

}  // namespace superzip
