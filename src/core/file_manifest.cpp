#include "core/file_manifest.hpp"

#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <algorithm>

namespace superzip {
namespace {

// Purpose: Recursively add one filesystem path to an archive manifest.
// Inputs: `manifest` is mutated, `root_parent` is the base used for relative names, and `path` is the current filesystem node.
// Outputs: Adds directory/file entries and byte totals; throws on symlinks, unsupported file types, unsafe names, or filesystem errors.
void add_path(
    Manifest& manifest,
    const std::filesystem::path& root_parent,
    const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        throw ArchiveError("cannot inspect source path: " + path.string() + ": " + ec.message());
    }
    if (std::filesystem::is_symlink(status)) {
        throw SecurityError("refusing to archive symbolic link: " + path.string());
    }

    const auto relative = std::filesystem::relative(path, root_parent, ec);
    if (ec) {
        throw ArchiveError("cannot create relative archive path: " + path.string());
    }

    if (std::filesystem::is_directory(status)) {
        manifest.entries.push_back(ManifestEntry{
            .source_path = path,
            .archive_path = normalize_entry_name(relative) + "/",
            .directory = true,
            .size = 0,
        });
        std::vector<std::filesystem::path> children;
        for (const auto& child : std::filesystem::directory_iterator(path)) {
            children.push_back(child.path());
        }
        std::ranges::sort(children);
        for (const auto& child : children) {
            add_path(manifest, root_parent, child);
        }
        return;
    }

    if (!std::filesystem::is_regular_file(status)) {
        throw SecurityError("refusing to archive non-regular file: " + path.string());
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw ArchiveError("cannot read source size: " + path.string() + ": " + ec.message());
    }
    manifest.entries.push_back(ManifestEntry{
        .source_path = path,
        .archive_path = normalize_entry_name(relative),
        .directory = false,
        .size = size,
    });
    manifest.total_file_bytes += size;
    ++manifest.file_count;
}

}  // namespace

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
        add_path(manifest, parent, absolute);
    }
    std::ranges::sort(manifest.entries, [](const ManifestEntry& lhs, const ManifestEntry& rhs) {
        return lhs.archive_path < rhs.archive_path;
    });
    return manifest;
}

}  // namespace superzip
