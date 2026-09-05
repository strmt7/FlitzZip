#include "core/file_manifest.hpp"

#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace superzip {

class ManifestSourceLockState {
  public:
#ifdef _WIN32
    std::vector<HANDLE> handles;

    // Purpose: Release every source-file and parent-chain handle held by this lock.
    // Inputs: None.
    // Outputs: Closes handles from leaf to root without throwing.
    ~ManifestSourceLockState() {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            CloseHandle(*it);
        }
    }
#endif
};

namespace {

// Purpose: Ensure another manifest entry can be added without unbounded memory growth.
// Inputs: `manifest` is the current archive plan and `path` labels the entry for diagnostics.
// Outputs: Returns normally while the manifest stays inside SuperZip resource limits; throws `ArchiveError` otherwise.
void reject_manifest_entry_overflow(const Manifest& manifest, const std::filesystem::path& path) {
    if (manifest.entries.size() >= kMaxArchiveEntries) {
        throw ArchiveError("too many input entries for one archive: " + path.string());
    }
}

// Purpose: Add file bytes to the manifest total without unsigned wraparound.
// Inputs: `manifest` is the current archive plan and `size` is the new regular-file byte count.
// Outputs: Mutates `manifest.total_file_bytes`; throws `ArchiveError` when the total would overflow.
void add_total_file_bytes(Manifest& manifest, std::uint64_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - manifest.total_file_bytes) {
        throw ArchiveError("input file sizes exceed SuperZip accounting limits");
    }
    manifest.total_file_bytes += size;
}

// Purpose: Charge one normalized archive name to the aggregate manifest metadata budget.
// Inputs: `manifest` owns the running total and `archive_path` is the retained normalized name.
// Outputs: Updates the total or throws before the aggregate path budget is exceeded.
void add_manifest_path_bytes(Manifest& manifest, std::string_view archive_path) {
    if (archive_path.size() > kMaxArchivePathMetadataBytes - manifest.path_metadata_bytes) {
        throw ArchiveError("source path metadata exceeds SuperZip resource limits");
    }
    manifest.path_metadata_bytes += archive_path.size();
}

#ifdef _WIN32
// Purpose: Detect Windows reparse points before source-tree traversal can cross out of the selected root.
// Inputs: `path` is an existing source path supplied to the archive manifest builder.
// Outputs: Returns true for symlinks, junctions, mount points, and other reparse entries; throws on inspection failure.
bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw ArchiveError("cannot inspect source path attributes: " + path.string());
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

// Purpose: Open one existing directory without following or permitting replacement of reparses.
// Inputs: `directory` is an absolute source-parent path.
// Outputs: Returns an owned non-delete-sharing handle or throws.
HANDLE open_source_directory(const std::filesystem::path& directory) {
    const auto text = directory.wstring();
    const auto handle = CreateFileW(text.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw SecurityError("cannot pin source directory: " + directory.string());
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        CloseHandle(handle);
        throw SecurityError("source parent chain contains an unsafe object: " + directory.string());
    }
    return handle;
}

// Purpose: Open and pin every existing source-parent component from root to leaf.
// Inputs: `parent` is the normalized absolute parent directory.
// Outputs: Returns ordered owned handles or closes partial state and throws.
std::vector<HANDLE> pin_source_parent_chain(const std::filesystem::path& parent) {
    std::vector<HANDLE> handles;
    try {
        auto current = parent.root_path();
        if (current.empty())
            throw SecurityError("source path has no absolute root");
        handles.push_back(open_source_directory(current));
        for (const auto& component : parent.relative_path()) {
            if (component.empty() || component == L".")
                continue;
            if (component == L"..")
                throw SecurityError("source path contains parent traversal");
            current /= component;
            handles.push_back(open_source_directory(current));
        }
        return handles;
    } catch (...) {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it)
            CloseHandle(*it);
        throw;
    }
}

// Purpose: Query stable identity, size, and write-time metadata from an open regular source file.
// Inputs: `handle` is an open non-reparse source file.
// Outputs: Returns the identity snapshot or throws when metadata cannot be proven.
SourceFileIdentity source_identity_from_handle(HANDLE handle) {
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 ||
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0 ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == 0 ||
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == 0 ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        standard.EndOfFile.QuadPart < 0) {
        throw SecurityError("source file identity cannot be verified");
    }
    SourceFileIdentity result;
    result.volume_serial = id.VolumeSerialNumber;
    std::memcpy(result.file_id.data(), id.FileId.Identifier, result.file_id.size());
    result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    result.last_write_time = basic.LastWriteTime.QuadPart;
    result.available = true;
    return result;
}

// Purpose: Open a source file with mutation/delete sharing denied while parent handles remain pinned.
// Inputs: `path` is a normalized absolute regular-file path and `state` receives all owned handles.
// Outputs: Returns the file identity and appends its handle, or throws after cleaning state through RAII.
SourceFileIdentity open_locked_source(const std::filesystem::path& path, ManifestSourceLockState& state) {
    state.handles = pin_source_parent_chain(path.parent_path());
    const auto text = path.wstring();
    const auto handle = CreateFileW(text.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw SecurityError("cannot lock source file: " + path.string());
    }
    state.handles.push_back(handle);
    return source_identity_from_handle(handle);
}

// Purpose: Compare two source identity snapshots without pathname assumptions.
// Inputs: `expected` comes from manifest construction and `actual` from the locked writer-time object.
// Outputs: Returns true only when object, size, and write timestamp all match.
bool source_identity_matches(const SourceFileIdentity& expected, const SourceFileIdentity& actual) {
    return expected.available && actual.available && expected.volume_serial == actual.volume_serial &&
           expected.file_id == actual.file_id && expected.size == actual.size &&
           expected.last_write_time == actual.last_write_time;
}
#endif

// Purpose: Recursively add one filesystem path to an archive manifest.
// Inputs: `manifest` is mutated, `root_parent` is the base used for relative names, and `path` is the current
// filesystem node. Outputs: Adds directory/file entries and byte totals; throws on symlinks, unsupported file types,
// unsafe names, or filesystem errors.
void add_path(Manifest& manifest, const std::filesystem::path& root_parent, const std::filesystem::path& path,
              std::uint32_t depth) {
    if (depth > kMaxSourceDirectoryDepth) {
        throw ArchiveError("source directory depth exceeds SuperZip resource limits: " + path.string());
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        throw ArchiveError("cannot inspect source path: " + path.string() + ": " + ec.message());
    }
    if (std::filesystem::is_symlink(status)) {
        throw SecurityError("refusing to archive symbolic link: " + path.string());
    }
#ifdef _WIN32
    if (is_reparse_point(path)) {
        throw SecurityError("refusing to archive reparse point: " + path.string());
    }
#endif

    const auto relative = std::filesystem::relative(path, root_parent, ec);
    if (ec) {
        throw ArchiveError("cannot create relative archive path: " + path.string());
    }

    if (std::filesystem::is_directory(status)) {
        reject_manifest_entry_overflow(manifest, path);
        const auto archive_path = normalize_entry_name(relative) + "/";
        add_manifest_path_bytes(manifest, archive_path);
        manifest.entries.push_back(ManifestEntry{
            .source_path = path,
            .archive_path = archive_path,
            .directory = true,
            .size = 0,
        });
        std::vector<std::filesystem::path> children;
        for (const auto& child : std::filesystem::directory_iterator(path)) {
            if (children.size() >= kMaxArchiveEntries) {
                throw ArchiveError("directory fanout exceeds SuperZip resource limits: " + path.string());
            }
            children.push_back(child.path());
        }
        std::ranges::sort(children);
        for (const auto& child : children) {
            add_path(manifest, root_parent, child, depth + 1U);
        }
        return;
    }

    if (!std::filesystem::is_regular_file(status)) {
        throw SecurityError("refusing to archive non-regular file: " + path.string());
    }

    SourceFileIdentity identity;
#ifdef _WIN32
    auto source_state = std::make_shared<ManifestSourceLockState>();
    identity = open_locked_source(path, *source_state);
#else
    identity.size = std::filesystem::file_size(path, ec);
    identity.available = !ec;
#endif
    if (!identity.available)
        throw ArchiveError("cannot capture source file identity: " + path.string());
    reject_manifest_entry_overflow(manifest, path);
    const auto archive_path = normalize_entry_name(relative);
    add_manifest_path_bytes(manifest, archive_path);
    manifest.entries.push_back(ManifestEntry{
        .source_path = path,
        .archive_path = archive_path,
        .directory = false,
        .size = identity.size,
        .identity = identity,
    });
    add_total_file_bytes(manifest, identity.size);
    ++manifest.file_count;
}

}  // namespace

// Purpose: Convert source roots into deterministic bounded entries with captured regular-file identities.
// Inputs: `sources` are user-selected paths; reparses and unsupported types are rejected.
// Outputs: Returns sorted manifest metadata or throws before an unsafe or over-budget source is admitted.
Manifest build_manifest(const std::vector<std::filesystem::path>& sources) {
    if (sources.empty()) {
        throw ArchiveError("at least one source path is required");
    }
    Manifest manifest;
    for (const auto& source : sources) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(source, ec);
        if (ec) {
            throw ArchiveError("cannot resolve source path: " + source.string());
        }
        if (!std::filesystem::exists(absolute)) {
            throw ArchiveError("source path does not exist: " + source.string());
        }
        const auto parent = absolute.parent_path();
        add_path(manifest, parent, absolute, 0U);
    }
    std::ranges::sort(manifest.entries, [](const ManifestEntry& lhs, const ManifestEntry& rhs) {
        return lhs.archive_path < rhs.archive_path;
    });
    return manifest;
}

// Purpose: Pin and verify a regular source file before a format writer reopens its pathname.
// Inputs: `entry` is a non-directory manifest entry with captured object identity.
// Outputs: Returns an RAII lock that prevents source/parent replacement and mutation until destroyed, or throws.
ManifestSourceLock lock_manifest_source(const ManifestEntry& entry) {
    if (entry.directory || !entry.identity.available) {
        throw SecurityError("manifest source lock requires a verified regular file");
    }
    auto state = std::make_shared<ManifestSourceLockState>();
#ifdef _WIN32
    const auto actual = open_locked_source(entry.source_path, *state);
    if (!source_identity_matches(entry.identity, actual)) {
        throw SecurityError("source file changed after manifest validation: " + entry.source_path.string());
    }
#else
    std::error_code error;
    if (std::filesystem::file_size(entry.source_path, error) != entry.identity.size || error) {
        throw SecurityError("source file changed after manifest validation: " + entry.source_path.string());
    }
#endif
    return ManifestSourceLock(std::move(state));
}

// Purpose: Pin and verify one regular source file for a complete multi-stage operation.
// Inputs: `path` identifies an existing non-reparse regular file.
// Outputs: Returns its normalized path, exact size, and an RAII lock that prevents mutation or replacement.
PinnedSourceFile pin_source_file(const std::filesystem::path& path) {
    const auto manifest = build_manifest({path});
    if (manifest.entries.size() != 1U || manifest.entries.front().directory) {
        throw SecurityError("source session requires exactly one regular file: " + path.string());
    }
    const auto& entry = manifest.entries.front();
    return PinnedSourceFile(entry.source_path, entry.size, lock_manifest_source(entry));
}

}  // namespace superzip
