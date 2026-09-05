#include "core/file_publish.hpp"

#include "core/file_manifest.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#endif

namespace superzip {

class FilePublishReservationState {
  public:
    std::filesystem::path target;
    std::filesystem::path directory;
    std::filesystem::path file;
    bool active = true;

#ifdef _WIN32
    std::vector<HANDLE> directory_handles;
    HANDLE temporary_directory_handle = INVALID_HANDLE_VALUE;

    // Purpose: Return the pinned final parent directory handle.
    // Inputs: None; `directory_handles` must contain the reserved target parent chain.
    // Outputs: Returns the final parent handle or throws for an invalid reservation.
    HANDLE parent_handle() const {
        if (directory_handles.empty()) {
            throw ArchiveError("publication reservation has no pinned parent directory");
        }
        return directory_handles.back();
    }

    // Purpose: Release the private staging directory lock before cleanup.
    // Inputs: None.
    // Outputs: Closes the staging handle once and leaves parent-chain locks active.
    void release_temporary_directory() noexcept {
        if (temporary_directory_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(temporary_directory_handle);
            temporary_directory_handle = INVALID_HANDLE_VALUE;
        }
    }
#endif

    // Purpose: Release all operating-system handles owned by the reservation.
    // Inputs: None.
    // Outputs: Closes staging and parent handles without throwing.
    ~FilePublishReservationState() {
#ifdef _WIN32
        release_temporary_directory();
        for (auto it = directory_handles.rbegin(); it != directory_handles.rend(); ++it) {
            CloseHandle(*it);
        }
#endif
    }
};

namespace {

// Purpose: Convert a path to a normalized absolute path for reservation identity checks.
// Inputs: `path` is a caller-selected output or staging path.
// Outputs: Returns an absolute lexical path or throws when resolution fails.
std::filesystem::path normalized_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw ArchiveError("unable to resolve output path: " + error.message());
    }
    return absolute.lexically_normal();
}

struct QuarantineTreeEntry {
    std::filesystem::path source;
    std::string relative_path;
    bool directory = false;
    std::uint32_t depth = 0;
};

// Purpose: Convert a quarantine-relative path to a strict archive-style key for destination validation.
// Inputs: `root` is the protected staging root and `path` is one descendant.
// Outputs: Returns a slash-separated relative path or throws when the object is outside the quarantine root.
std::string quarantine_relative_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty() || relative.is_absolute()) {
        throw SecurityError("quarantine entry cannot be relativized");
    }
    const auto utf8 = relative.generic_u8string();
    const std::string result(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    static_cast<void>(normalize_archive_path_key(result));
    return result;
}

// Purpose: Inventory one protected extraction tree under archive entry, depth, and path-metadata limits.
// Inputs: `root` is the private quarantine directory populated by an archive adapter.
// Outputs: Returns deterministic directory/file entries and rejects reparses, unsupported objects, or excess work.
std::vector<QuarantineTreeEntry> inventory_quarantine_tree(const std::filesystem::path& root) {
    struct PendingDirectory {
        std::filesystem::path path;
        std::uint32_t depth = 0;
    };
    std::vector<PendingDirectory> pending{{root, 0U}};
    std::vector<QuarantineTreeEntry> entries;
    std::uint64_t path_bytes = 0;
    while (!pending.empty()) {
        const auto current = std::move(pending.back());
        pending.pop_back();
        if (current.depth > kMaxArchivePathComponents) {
            throw ArchiveError("quarantine directory depth exceeds SuperZip resource limits");
        }
        std::error_code error;
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator iterator(current.path, error), end; !error && iterator != end;
             iterator.increment(error)) {
            if (children.size() >= kMaxArchiveEntries) {
                throw ArchiveError("quarantine directory fanout exceeds SuperZip resource limits");
            }
            children.push_back(iterator->path());
        }
        if (error) {
            throw ArchiveError("cannot enumerate complete extraction quarantine: " + error.message());
        }
        std::ranges::sort(children);
        for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
            if (entries.size() >= kMaxArchiveEntries) {
                throw ArchiveError("quarantine entry count exceeds SuperZip resource limits");
            }
            const auto status = std::filesystem::symlink_status(*iterator, error);
            if (error || std::filesystem::is_symlink(status)) {
                throw SecurityError("extraction quarantine contains an unsafe object");
            }
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(iterator->c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                throw SecurityError("extraction quarantine contains a reparse point");
            }
#endif
            const auto relative = quarantine_relative_path(root, *iterator);
            path_bytes =
                checked_add_archive_path_metadata_bytes(path_bytes, relative.size(), "quarantine path metadata");
            const bool directory = std::filesystem::is_directory(status);
            if (!directory && !std::filesystem::is_regular_file(status)) {
                throw SecurityError("extraction quarantine contains an unsupported object");
            }
            entries.push_back(QuarantineTreeEntry{
                .source = *iterator,
                .relative_path = relative,
                .directory = directory,
                .depth = current.depth + 1U,
            });
            if (directory) {
                pending.push_back(PendingDirectory{.path = *iterator, .depth = current.depth + 1U});
            }
        }
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) {
        if (left.directory != right.directory)
            return left.directory > right.directory;
        if (left.directory && left.depth != right.depth)
            return left.depth < right.depth;
        return left.relative_path < right.relative_path;
    });
    return entries;
}

// Purpose: Copy one locked quarantine file into a verified per-file publication transaction.
// Inputs: `source` is a protected ordinary file, `target` is its validated final path, and `overwrite` is policy.
// Outputs: Publishes the exact source bytes atomically or throws without exposing a partial target.
void publish_quarantine_file(const std::filesystem::path& source, const std::filesystem::path& target, bool overwrite) {
    const auto pinned_source = pin_source_file(source);
    FilePublishTransaction transaction(target);
    std::ifstream input(pinned_source.path(), std::ios::binary);
    std::ofstream output(transaction.staging_path(), std::ios::binary);
    if (!input || !output) {
        throw ArchiveError("cannot open extraction quarantine publication streams");
    }
    std::vector<char> buffer(1024U * 1024U);
    std::uint64_t copied = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
            copied += static_cast<std::uint64_t>(count);
        }
    }
    if (input.bad() || !output || copied != pinned_source.size()) {
        throw ArchiveError("extraction quarantine file changed or could not be copied completely");
    }
    output.close();
    if (!output) {
        throw ArchiveError("cannot finalize extraction quarantine publication");
    }
    transaction.commit(overwrite);
}

#ifdef _WIN32

// Purpose: Close a Windows handle at scope exit.
// Inputs: `handle` is null, invalid, or an owned kernel handle.
// Outputs: Closes a valid handle exactly once.
class ScopedHandle {
  public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    HANDLE get() const noexcept {
        return handle_;
    }
    HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Purpose: Query one variable-sized token information class.
// Inputs: `token` is a readable process token and `information_class` selects the record.
// Outputs: Returns the complete token information bytes or throws.
std::vector<std::byte> query_token_information(HANDLE token, TOKEN_INFORMATION_CLASS information_class) {
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token, information_class, nullptr, 0, &size));
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw ArchiveError("unable to query process security token size");
    }
    std::vector<std::byte> bytes(size);
    if (GetTokenInformation(token, information_class, bytes.data(), size, &size) == 0) {
        throw ArchiveError("unable to query process security token");
    }
    return bytes;
}

// Purpose: Return the current process user SID as an SDDL string.
// Inputs: `token` is the current process token.
// Outputs: Returns a stable SID string or throws.
std::wstring token_user_sid(HANDLE token) {
    const auto bytes = query_token_information(token, TokenUser);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
    LPWSTR sid_text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid_text) == 0) {
        throw ArchiveError("unable to encode process user SID");
    }
    const std::wstring result(sid_text);
    LocalFree(sid_text);
    return result;
}

// Purpose: Map the current token integrity RID to an SDDL mandatory-label alias.
// Inputs: `token` is the current process token.
// Outputs: Returns LW, ME, HI, or SI so lower-integrity writers can be denied.
std::wstring token_integrity_alias(HANDLE token) {
    const auto bytes = query_token_information(token, TokenIntegrityLevel);
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data());
    auto* sid = const_cast<PSID>(label->Label.Sid);
    const auto count = *GetSidSubAuthorityCount(sid);
    const auto rid = *GetSidSubAuthority(sid, count - 1U);
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID)
        return L"SI";
    if (rid >= SECURITY_MANDATORY_HIGH_RID)
        return L"HI";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID)
        return L"ME";
    return L"LW";
}

// Purpose: Build a protected DACL and no-write-up label for a private publication directory.
// Inputs: None; identity and integrity are read from the current process token.
// Outputs: Returns a LocalAlloc-owned security descriptor or throws.
std::unique_ptr<void, decltype(&LocalFree)> make_private_directory_security_descriptor() {
    HANDLE raw_token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == 0) {
        throw ArchiveError("unable to open process security token");
    }
    ScopedHandle token(raw_token);
    const auto sddl = L"D:P(A;;FA;;;" + token_user_sid(token.get()) + L")(A;;FA;;;SY)(A;;FA;;;BA)S:(ML;;NW;;;" +
                      token_integrity_alias(token.get()) + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
        0) {
        throw ArchiveError("unable to build private publication security descriptor");
    }
    return {descriptor, &LocalFree};
}

// Purpose: Open and pin one directory without following a reparse point.
// Inputs: `directory` is one absolute path component chain.
// Outputs: Returns an owned non-delete-sharing handle or throws for missing, non-directory, or reparse objects.
HANDLE open_pinned_directory(const std::filesystem::path& directory, bool publication_parent = false) {
    const auto text = directory.wstring();
    auto access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (publication_parent) {
        access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    }
    ScopedHandle handle(CreateFileW(text.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        throw SecurityError("unable to pin output directory: " + directory.string() + " (Windows error " +
                            std::to_string(error) + ")");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        throw SecurityError("output directory chain contains an unsafe object: " + directory.string());
    }
    return handle.release();
}

// Purpose: Create missing components and pin a complete directory chain.
// Inputs: `directory` is a normalized absolute directory path.
// Outputs: Returns handles for every component, ordered root to leaf, or throws without following reparses.
std::vector<HANDLE> create_and_pin_directory_chain(const std::filesystem::path& directory) {
    const auto root = directory.root_path();
    if (root.empty()) {
        throw SecurityError("output directory does not have an absolute root");
    }
    std::vector<HANDLE> handles;
    try {
        auto current = root;
        handles.push_back(open_pinned_directory(current));
        for (const auto& component : directory.relative_path()) {
            if (component.empty() || component == L".")
                continue;
            if (component == L"..")
                throw SecurityError("output directory contains parent traversal");
            current /= component;
            const auto text = current.wstring();
            if (CreateDirectoryW(text.c_str(), nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
                throw ArchiveError("unable to create output directory: " + current.string());
            }
            handles.push_back(open_pinned_directory(current));
        }
        CloseHandle(handles.back());
        handles.back() = open_pinned_directory(directory, true);
        return handles;
    } catch (...) {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it)
            CloseHandle(*it);
        throw;
    }
}

// Purpose: Return a cryptographically unpredictable publication-directory suffix.
// Inputs: None.
// Outputs: Returns 128 random bits encoded as lowercase hexadecimal or throws.
std::wstring random_publication_suffix() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0) {
        throw ArchiveError("unable to generate a private publication name");
    }
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0FU]);
    }
    return result;
}

// Purpose: Create and pin a private staging directory beside the final target.
// Inputs: `target` is the normalized absolute final path and `state` owns parent locks.
// Outputs: Populates the exact random directory/file paths and staging handle or throws.
void create_private_staging_directory(const std::filesystem::path& target, FilePublishReservationState& state) {
    const auto descriptor = make_private_directory_security_descriptor();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
    for (std::uint32_t attempt = 0; attempt < 128U; ++attempt) {
        auto directory = target;
        directory += L".sztmp-" + random_publication_suffix();
        const auto text = directory.wstring();
        if (CreateDirectoryW(text.c_str(), &security) == 0) {
            if (GetLastError() == ERROR_ALREADY_EXISTS)
                continue;
            throw ArchiveError("unable to reserve private publication directory: " + target.string());
        }
        try {
            state.temporary_directory_handle = open_pinned_directory(directory);
            state.directory = directory;
            state.file = directory / "payload.tmp";
            return;
        } catch (...) {
            RemoveDirectoryW(text.c_str());
            throw;
        }
    }
    throw ArchiveError("unable to reserve a unique publication directory: " + target.string());
}

// Purpose: Validate a staged payload and open the exact file for handle-relative rename.
// Inputs: `temporary` is the state-bound staging file path.
// Outputs: Returns an owned read/delete handle or throws for reparse, directory, or open failures.
HANDLE open_verified_payload(const ReservedFilePublishTarget& temporary) {
    const auto text = temporary.file.wstring();
    ScopedHandle handle(CreateFileW(
        text.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        throw ArchiveError("unable to open verified publication payload");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == 0 ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        throw SecurityError("publication payload is not an ordinary file");
    }
    return handle.release();
}

// Purpose: Rename an open payload through a path whose complete parent chain remains pinned.
// Inputs: `payload` is the exact staged file, `target` has pinned reparse-free parents, and `overwrite` controls
// replacement. Outputs: Atomically moves the file or throws while all parent identities remain locked.
void rename_payload_to_pinned_path(HANDLE payload, const std::filesystem::path& target, bool overwrite) {
    const auto filename = target.filename().wstring();
    if (filename.empty() || filename == L"." || filename == L".." || filename.find(L':') != std::wstring::npos) {
        throw SecurityError("final output filename is unsafe");
    }
    const auto target_text = target.wstring();
    const auto filename_bytes = target_text.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + filename_bytes + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = overwrite ? TRUE : FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(filename_bytes);
    std::memcpy(rename->FileName, target_text.data(), filename_bytes);
    if (SetFileInformationByHandle(payload, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) == 0) {
        const auto error = GetLastError();
        if (!overwrite &&
            (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS || error == ERROR_ACCESS_DENIED)) {
            throw SecurityError("refusing to overwrite existing file: " + target.string());
        }
        throw ArchiveError("failed to publish verified output: " + target.string() + " (Windows error " +
                           std::to_string(error) + ")");
    }
}

#endif

}  // namespace

// Purpose: Create and validate a directory chain without traversing Windows reparse points.
// Inputs: `directory` is the absolute or relative directory path to create and validate.
// Outputs: Creates missing directories or throws while the chain cannot be proven reparse-free.
void create_verified_directories(const std::filesystem::path& directory) {
    const auto absolute = normalized_absolute_path(directory);
#ifdef _WIN32
    auto handles = create_and_pin_directory_chain(absolute);
    for (auto it = handles.rbegin(); it != handles.rend(); ++it)
        CloseHandle(*it);
#else
    std::filesystem::create_directories(absolute);
#endif
}

// Purpose: Reserve a private temporary file target beside a final output file.
// Inputs: `target` is the final file path that will later receive verified bytes.
// Outputs: Returns a state-bound private directory and payload path, or throws on reservation failure.
ReservedFilePublishTarget reserve_file_publish_target(const std::filesystem::path& target) {
    const auto absolute_target = normalized_absolute_path(target);
    if (absolute_target.filename().empty()) {
        throw SecurityError("publication target does not name a file");
    }
    auto state = std::make_shared<FilePublishReservationState>();
    state->target = absolute_target;
#ifdef _WIN32
    state->directory_handles = create_and_pin_directory_chain(absolute_target.parent_path());
    create_private_staging_directory(absolute_target, *state);
#else
    std::filesystem::create_directories(absolute_target.parent_path());
    for (std::uint32_t attempt = 0; attempt < 128U; ++attempt) {
        auto directory = absolute_target;
        directory += ".sztmp-" + std::to_string(attempt);
        if (std::filesystem::create_directory(directory)) {
            state->directory = directory;
            state->file = directory / "payload.tmp";
            break;
        }
    }
    if (state->directory.empty())
        throw ArchiveError("unable to reserve temporary output path");
#endif
    return ReservedFilePublishTarget{.directory = state->directory, .file = state->file, .state = std::move(state)};
}

// Purpose: Remove a reserved private publication tree after success or failure.
// Inputs: `temporary` is the reserved temporary directory and payload path.
// Outputs: Releases reservation locks and best-effort removes only the state-bound private tree.
void cleanup_file_publish_target(const ReservedFilePublishTarget& temporary) {
    if (!temporary.state || !temporary.state->active || temporary.directory != temporary.state->directory ||
        temporary.file != temporary.state->file) {
        return;
    }
#ifdef _WIN32
    temporary.state->release_temporary_directory();
#endif
    std::error_code ignored;
    std::filesystem::remove_all(temporary.directory, ignored);
    temporary.state->active = false;
}

// Purpose: Publish a fully verified temporary file into its final path.
// Inputs: `temporary` owns the verified payload and pinned parent, `target` is its reserved final path, and `overwrite`
// controls replacement. Outputs: Atomically renames the exact payload handle relative to the pinned parent, or throws
// without deleting the caller-owned temporary tree.
void commit_verified_file(const ReservedFilePublishTarget& temporary, const std::filesystem::path& target,
                          bool overwrite) {
    if (!temporary.state || !temporary.state->active || temporary.directory != temporary.state->directory ||
        temporary.file != temporary.state->file || normalized_absolute_path(target) != temporary.state->target) {
        throw SecurityError("publication target does not match its reservation");
    }
#ifdef _WIN32
    ScopedHandle payload(open_verified_payload(temporary));
    if (FlushFileBuffers(payload.get()) == 0) {
        throw ArchiveError("failed to flush verified publication payload");
    }
    static_cast<void>(temporary.state->parent_handle());
    rename_payload_to_pinned_path(payload.get(), temporary.state->target, overwrite);
#else
    if (overwrite) {
        std::filesystem::rename(temporary.file, temporary.state->target);
        return;
    }
    std::error_code error;
    std::filesystem::create_hard_link(temporary.file, temporary.state->target, error);
    if (error)
        throw SecurityError("refusing to overwrite existing file: " + target.string());
    std::filesystem::remove(temporary.file, error);
#endif
}

// Purpose: Reserve a private staging file and pin the final target parent for one write transaction.
// Inputs: `target` is the final output file path.
// Outputs: Constructs an active transaction or throws before caller writes begin.
FilePublishTransaction::FilePublishTransaction(const std::filesystem::path& target)
    : target_(normalized_absolute_path(target)), temporary_(reserve_file_publish_target(target_)) {}

// Purpose: Clean an uncommitted private staging tree.
// Inputs: The transaction's current state.
// Outputs: Best-effort cleanup without throwing.
FilePublishTransaction::~FilePublishTransaction() {
    if (!committed_) {
        cleanup_file_publish_target(temporary_);
    }
}

// Purpose: Return the private path to which the caller must write complete verified bytes.
// Inputs: None.
// Outputs: Returns a stable path valid until commit or destruction.
const std::filesystem::path& FilePublishTransaction::staging_path() const noexcept {
    return temporary_.file;
}

// Purpose: Atomically publish the staged file to its state-bound final target.
// Inputs: `overwrite` controls whether an existing final file may be replaced.
// Outputs: Commits and cleans staging state, or throws while retaining destructor cleanup.
void FilePublishTransaction::commit(bool overwrite) {
    if (committed_) {
        throw SecurityError("publication transaction is already committed");
    }
    commit_verified_file(temporary_, target_, overwrite);
    cleanup_file_publish_target(temporary_);
    committed_ = true;
}

// Purpose: Reserve a private extraction quarantine beneath a verified destination root.
// Inputs: `destination` is the final directory into which clean files will be merged.
// Outputs: Constructs an active transaction with a protected staging tree or throws before extraction starts.
DirectoryPublishTransaction::DirectoryPublishTransaction(const std::filesystem::path& destination)
    : destination_(normalized_absolute_path(destination)),
      quarantine_(reserve_file_publish_target(destination_ / ".superzip-quarantine")) {}

// Purpose: Remove unpublished quarantine bytes on every failure path.
// Inputs: Uses this transaction's reservation.
// Outputs: Best-effort removes only the state-bound private tree.
DirectoryPublishTransaction::~DirectoryPublishTransaction() {
    if (!published_) {
        cleanup_file_publish_target(quarantine_);
    }
}

// Purpose: Return the private directory that archive adapters may populate.
// Inputs: None.
// Outputs: Returns a stable protected path valid until publish or destruction.
const std::filesystem::path& DirectoryPublishTransaction::staging_directory() const noexcept {
    return quarantine_.directory;
}

// Purpose: Merge every verified staged directory and file into the final destination.
// Inputs: `overwrite` controls replacement of existing final files.
// Outputs: Publishes only ordinary non-reparse files through per-file transactions and removes quarantine state.
void DirectoryPublishTransaction::publish(bool overwrite) {
    if (published_) {
        throw ArchiveError("extraction quarantine transaction was already published");
    }
    const auto entries = inventory_quarantine_tree(quarantine_.directory);
    for (const auto& entry : entries) {
        const auto target = safe_join_archive_path(destination_, entry.relative_path, ArchivePathEncoding::Utf8);
        std::error_code error;
        if (!overwrite && !entry.directory && std::filesystem::exists(target, error)) {
            throw SecurityError("refusing to overwrite existing file: " + target.string());
        }
        if (error) {
            throw ArchiveError("cannot inspect extraction publication target: " + error.message());
        }
    }
    for (const auto& entry : entries) {
        if (entry.directory) {
            create_verified_directories(
                safe_join_archive_path(destination_, entry.relative_path, ArchivePathEncoding::Utf8));
        }
    }
    for (const auto& entry : entries) {
        if (!entry.directory) {
            const auto target = safe_join_archive_path(destination_, entry.relative_path, ArchivePathEncoding::Utf8);
            publish_quarantine_file(entry.source, target, overwrite);
        }
    }
    cleanup_file_publish_target(quarantine_);
    published_ = true;
}

}  // namespace superzip
