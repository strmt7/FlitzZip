#pragma once

#include "app/settings_json.hpp"
#include "core/file_publish.hpp"
#include "core/result.hpp"
#include "core/path_text.hpp"
#include "core/windows_handle.hpp"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string_view>

namespace flitzzip::app {

// Serializes cooperating writers across processes. The empty lock file remains
// in the profile so releasing a lock cannot race a second writer's file open.
class SettingsWriterLock final {
  public:
    // Purpose: Acquire one profile's nonblocking writer lock without following a redirected lock file.
    // Inputs: Trusted settings path with an existing parent. Outputs: Exclusive handle or a reported busy/I/O error.
    explicit SettingsWriterLock(const std::filesystem::path& path)
        : handle_(CreateFileW(windows_api_path(path.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        if (handle_.get() == INVALID_HANDLE_VALUE)
            throw ArchiveError("settings are busy or cannot be locked");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle_.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
            throw ArchiveError("settings lock is not a regular file");
    }

  private:
    windows_detail::HandleOwner handle_;
};

// Purpose: Finish a bounded serialized settings write before the caller may publish its file.
// Inputs: The output stream and complete JSON produced by the settings serializer.
// Outputs: Writes and flushes every byte, or throws for invalid size, write failure or flush failure.
inline void write_settings_json_stream(std::ostream& output, std::string_view json) {
    if (json.empty() || json.size() > kMaxSettingsJsonBytes) {
        throw ArchiveError("serialized settings are empty or exceed the size limit");
    }
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.flush();
    if (!output) {
        throw ArchiveError("could not write complete settings file");
    }
}

// Purpose: Replace settings through the same private, pinned publication boundary used by archive writers.
// Inputs: Trusted per-user target and complete serialized settings JSON. Outputs: Publishes one closed file;
// failed saves preserve the preceding target and clean only this transaction's private staging area.
inline void publish_settings_json(const std::filesystem::path& path, std::string_view json) {
    FilePublishTransaction transaction(path);
    std::ofstream output(transaction.staging_path(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ArchiveError("could not open settings file for writing");
    }
    write_settings_json_stream(output, json);
    output.close();
    if (output.fail()) {
        throw ArchiveError("could not close complete settings file");
    }
    transaction.commit(true);
}

}  // namespace flitzzip::app
