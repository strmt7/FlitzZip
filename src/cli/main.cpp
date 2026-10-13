#include "cli/memory_benchmark.hpp"
#include "cli/memory_benchmark_source.hpp"
#include "core/host_memory_budget.hpp"
#include "ar/ar_adapter.hpp"
#include "arc/arc_adapter.hpp"
#include "arj/arj_adapter.hpp"
#include "base64/base64_adapter.hpp"
#include "bzip2/bzip2_adapter.hpp"
#include "cab/cab_adapter.hpp"
#include "core/archive.hpp"
#include "core/archive_format.hpp"
#include "core/checksum.hpp"
#include "cpio/cpio_adapter.hpp"
#include "core/defender_scan.hpp"
#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/integrity.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"
#include "gzip/gzip_adapter.hpp"
#include "gpu/gpu_codec.hpp"
#include "hqx/hqx_adapter.hpp"
#include "iso/iso_adapter.hpp"
#include "lha/lha_adapter.hpp"
#include "lzip/lzip_adapter.hpp"
#include "lzma/lzma_adapter.hpp"
#include "macbinary/macbinary_adapter.hpp"
#include "rpm/rpm_adapter.hpp"
#include "sevenzip/sevenzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "unix_compress/unix_compress_adapter.hpp"
#include "uue/uue_adapter.hpp"
#include "wim/wim_adapter.hpp"
#include "xar/xar_adapter.hpp"
#include "xxe/xxe_adapter.hpp"
#include "xz/xz_adapter.hpp"
#include "zstd/zstd_adapter.hpp"
#include "zip/zip_adapter.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>
#include <fcntl.h>
#include <io.h>

namespace {

constexpr std::string_view kBlockSizeUsage = "256|512|1024|2048|4096|8192|16384";

// Purpose: Preserve Windows Unicode arguments before parsing command options.
// Inputs: One UTF-16 argument from the wide-character CRT entry point.
// Outputs: Returns exact UTF-8 text; rejects invalid UTF-16 rather than using code-page replacement characters.
std::string encode_cli_argument(std::wstring_view argument) {
    if (argument.empty()) {
        return {};
    }
    if (argument.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw flitzzip::ArchiveError("command-line argument exceeds conversion limits");
    }
    const auto length = static_cast<int>(argument.size());
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw flitzzip::ArchiveError("command-line argument contains invalid Unicode");
    }
    std::string encoded(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), length, encoded.data(), size, nullptr,
                            nullptr) != size) {
        throw flitzzip::ArchiveError("command-line Unicode conversion failed");
    }
    return encoded;
}

// Purpose: Convert UTF-8 command arguments to native filesystem paths without the ANSI code page.
// Inputs: An argument value already encoded by the wide-character CLI entry point.
// Outputs: Returns the corresponding native path without changing its spelling or resolving it.
std::filesystem::path cli_path_argument(const std::string& argument) {
    return std::filesystem::path(std::u8string(argument.begin(), argument.end()));
}

// Purpose: Print command-line help for supported FlitzZip operations.
// Inputs: None.
// Outputs: Writes usage text to stdout.
void usage() {
    std::cout
        << "FlitzZip CLI\n"
        << "Usage:\n"
        << "  flitzzip_cli --help\n"
        << "  flitzzip_cli --version\n"
        << "  flitzzip_cli gpu-info\n"
        << "  flitzzip_cli gpu-diagnostic [--seconds <1-30>] [--buffer-mib <16-512>] [--inner-iterations <1-4096>]\n"
        << "  flitzzip_cli dependency-check\n"
        << "  flitzzip_cli formats\n"
        << "  flitzzip_cli identify <archive>\n"
        << "  flitzzip_cli memory-benchmark --size-mib <n> --profile "
           "Mixed|Compressible|Incompressible|RepeatedRecord|SparseRecord|LongSparseRecord|SegmentedRecords "
           "[--require-gpu|--force-cpu] [--workers <n>] [--block-size-kib <"
        << kBlockSizeUsage
        << ">] [--compression-level <1-9>] [--neutron-star] [--inflight <n>] [--decode-inflight <n>] [--plan-only]\n"
        << "  flitzzip_cli memory-benchmark --source-file <file> --source-sha256 <lowercase-hex> [--plan-only] "
           "[--require-gpu|--force-cpu] [--workers <n>] [--block-size-kib <"
        << kBlockSizeUsage
        << ">] [--compression-level <1-9>] [--neutron-star] [--inflight <n>] [--decode-inflight <n>]\n"
        << "  flitzzip_cli memory-benchmark --source-stdin --source-bytes <1-4294967295> "
           "--source-sha256 <lowercase-hex> [--neutron-star] [--plan-only] [the same corpus options]\n"
        << "  flitzzip_cli benchmark-suite [--size-mib <n>] [--profile "
           "Mixed|Compressible|Incompressible|RepeatedRecord|SparseRecord|LongSparseRecord|SegmentedRecords] "
           "[--workers "
           "<n>] [--block-size-kib <"
        << kBlockSizeUsage << ">] [--compression-level <1-9>] [--tune] [--tune-levels]\n"
        << "  flitzzip_cli compress --format flzip --output <archive> [--require-gpu|--force-cpu] [--workers <n>] "
           "[--inflight <n>] [--block-size-kib <"
        << kBlockSizeUsage
        << ">] [--compression-level <1-9>] [--neutron-star] "
           "[--verify-after-write] [--sha256] [--defender-scan] <path>...\n"
        << "  flitzzip_cli compress --format "
           "zip|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.zst|tzst|gz|gzip|bz2|bzip2|zst|zstd|z|compress|cpio|cpio.gz|"
           "cpgz|ar --output <archive> [--compression-level <1-9>] "
           "[--verify-after-write] [--sha256] [--defender-scan] <path>... (ZIP only)\n"
        << "  flitzzip_cli extract --format flzip --output <directory> [--require-gpu|--force-cpu] [--workers <n>] "
           "[--inflight <n>] [--overwrite] [--validate-before-publish] [--sha256] [--defender-scan] <archive.flzip>\n"
        << "  flitzzip_cli extract --format "
           "auto|zip|zipx|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.xz|txz|tar.lz|tlz|tar.zst|tzst|gz|gzip|bz2|bzip2|xz|lzma|"
           "lz|lzip|zst|zstd|z|compress|b64|base64|hqx|binhex|xxe|xxencode|uue|uu|macbinary|macbin|cab|iso|cpio|cpio."
           "gz|cpgz|ar|arj|arc|ark|deb|rpm|7z|lha|lzh|wim|swm|xar --output <directory> [--overwrite] [--sha256] "
           "[--defender-scan] [--validate-before-publish] [--name-encoding utf8|system] <archive>\n"
        << "  flitzzip_cli verify [--require-gpu|--force-cpu] [--workers <n>] [--inflight <n>] [--sha256] "
           "[--defender-scan] <archive.flzip|archive.zip|archive.zipx> (GPU options: FLZIP only)\n";
}

// Purpose: Convert byte/second statistics to MiB/s for display.
// Inputs: `bytes` is the processed byte count and `seconds` is elapsed wall time.
// Outputs: Returns MiB per second, or zero when elapsed time is zero.
double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
}

// Purpose: Compute an archive size ratio for benchmark records.
// Inputs: `input_bytes` and `output_bytes` are uncompressed and compressed byte counters.
// Outputs: Returns output/input, or zero when no input bytes exist.
double compression_ratio(std::uint64_t input_bytes, std::uint64_t output_bytes) {
    return input_bytes == 0 ? 0.0 : static_cast<double>(output_bytes) / static_cast<double>(input_bytes);
}

// Purpose: Print archive operation statistics in a stable key/value format.
// Inputs: `stats` is the completed operation result.
// Outputs: Writes one line to stdout; includes optional adapter phase timings only when measured.
void print_stats(const flitzzip::OperationStats& stats) {
    std::cout << "entries=" << stats.entries << " input_bytes=" << stats.input_bytes
              << " output_bytes=" << stats.output_bytes << " workers=" << stats.workers
              << " inflight_chunks=" << stats.inflight_chunks << " gpu_used=" << (stats.gpu_used ? "true" : "false")
              << " gpu_encode_chunks=" << stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << stats.gpu_runtime.kernel_ms << " gpu_h2d_bytes=" << stats.gpu_runtime.h2d_bytes
              << " gpu_d2h_bytes=" << stats.gpu_runtime.d2h_bytes
              << " gpu_device_allocation_bytes=" << stats.gpu_runtime.device_allocation_bytes
              << " gpu_host_pinned_allocation_bytes=" << stats.gpu_runtime.host_pinned_allocation_bytes
              << " gpu_host_pinned_output_bytes=" << stats.gpu_runtime.host_pinned_output_bytes
              << " gpu_pattern_blocks=" << stats.gpu_runtime.pattern_blocks
              << " gpu_prefix_blocks=" << stats.gpu_runtime.prefix_blocks
              << " gpu_dictionary_blocks=" << stats.gpu_runtime.dictionary_blocks
              << " gpu_sparse_pattern_blocks=" << stats.gpu_runtime.sparse_pattern_blocks
              << " seconds=" << stats.seconds
              << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
              << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes);
    if (stats.phases) {
        std::cout << " setup_seconds=" << stats.phases->setup_seconds
                  << " stream_seconds=" << stats.phases->stream_seconds
                  << " publication_seconds=" << stats.phases->publication_seconds;
    }
    std::cout << '\n';
}

// Purpose: Print the archive formats that FlitzZip recognizes and whether each is implemented.
// Inputs: None.
// Outputs: Writes one parseable line per real archive format.
void print_format_registry() {
    for (const auto& info : flitzzip::archive_format_registry()) {
        if (info.format == flitzzip::ArchiveFormat::Unknown || info.format == flitzzip::ArchiveFormat::Auto) {
            continue;
        }
        std::cout << "format=" << info.key << " display=\"" << info.display_name << "\""
                  << " extensions=\"" << info.extensions << "\""
                  << " can_create=" << (info.can_create ? "true" : "false")
                  << " can_extract=" << (info.can_extract ? "true" : "false")
                  << " gpu_native=" << (info.gpu_native ? "true" : "false")
                  << " bundled_native=" << (info.bundled_native ? "true" : "false") << "\n";
    }
}

// Purpose: Resolve a CLI format token and optional auto-detection into one concrete archive format.
// Inputs: `format_token` is the user token, `archive_path` is used only for `auto`, and `allow_auto` gates
// auto-detection. Outputs: Returns a concrete format; throws `ArchiveError` for unknown tokens or unsupported
// auto-detection.
flitzzip::ArchiveFormat resolve_cli_archive_format(const std::string& format_token,
                                                   const std::filesystem::path& archive_path, bool allow_auto) {
    const auto parsed = flitzzip::parse_archive_format_token(format_token);
    if (!parsed.has_value()) {
        throw flitzzip::ArchiveError("unknown archive format: " + format_token);
    }
    if (*parsed != flitzzip::ArchiveFormat::Auto) {
        return *parsed;
    }
    if (!allow_auto) {
        throw flitzzip::ArchiveError("archive format auto-detection is only supported for extraction and identify");
    }
    const auto detected = flitzzip::detect_archive_format(archive_path);
    if (detected == flitzzip::ArchiveFormat::Unknown) {
        throw flitzzip::ArchiveError("unable to detect archive format: " +
                                     flitzzip::path_diagnostic_utf8(archive_path));
    }
    return detected;
}

// Purpose: Reject recognized archive formats that FlitzZip can identify but cannot yet process.
// Inputs: `format` is a concrete archive format and `operation` is `create` or `extract`.
// Outputs: Returns normally for implemented formats; throws a clear non-implementation error otherwise.
void reject_unsupported_cli_format(flitzzip::ArchiveFormat format, std::string_view operation) {
    const auto& info = flitzzip::archive_format_info(format);
    const bool supported = operation == "create" ? info.can_create : info.can_extract;
    if (!supported) {
        throw flitzzip::ArchiveError(std::string("archive format recognized but not yet implemented for ") +
                                     std::string(operation) + ": " + info.key);
    }
}

// Purpose: Print SHA-256 integrity data for a file or directory target.
// Inputs: `path` is an existing file or directory and `key_prefix` scopes the stable key/value names.
// Outputs: Writes algorithm, target type, digest, and counter lines to stdout; throws if hashing fails.
void print_integrity_hash(const std::filesystem::path& path, std::string_view key_prefix = "integrity") {
    const auto hash = flitzzip::hash_path(path, flitzzip::IntegrityMode::Sha256);
    std::cout << key_prefix << "_algorithm=" << hash.algorithm << "\n";
    std::cout << key_prefix << "_target=" << hash.target << "\n";
    std::cout << key_prefix << "_sha256=" << hash.hex_digest << "\n";
    std::cout << key_prefix << "_bytes=" << hash.bytes_hashed << "\n";
    std::cout << key_prefix << "_files=" << hash.files_hashed << "\n";
    std::cout << key_prefix << "_directories=" << hash.directories_hashed << "\n";
}

// Purpose: Run and print an opt-in Microsoft Defender scan result.
// Inputs: `path` is the target file or directory and `block_if_detected` controls whether a non-clean attempted scan
// aborts the operation. Outputs: Writes scan state to stdout; throws `SecurityError` when Defender reports a scanned
// target is not clean or times out and blocking is requested.
void print_defender_scan(const std::filesystem::path& path, bool block_if_detected) {
    const auto scan = flitzzip::scan_with_windows_defender(path, flitzzip::DefenderScanMode::FullPath);
    std::cout << "defender_attempted=" << (scan.attempted ? "true" : "false") << "\n";
    std::cout << "defender_clean=" << (scan.clean ? "true" : "false") << "\n";
    std::cout << "defender_timed_out=" << (scan.timed_out ? "true" : "false") << "\n";
    std::cout << "defender_exit_code=" << scan.exit_code << "\n";
    if (block_if_detected) {
        flitzzip::require_clean_defender_scan(scan, path);
    }
}

// Purpose: Print AMD HIP dependency state in a stable machine-readable format.
// Inputs: `info` is the GPU/runtime status returned by the backend.
// Outputs: Writes one key/value block to stdout.
void print_gpu_info(const flitzzip::GpuInfo& info) {
    std::cout << "hip_compiled=" << (info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_kernel_loadable=" << (info.hip_kernel_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << info.runtime_name << "\n";
    std::cout << "hip_runtime_version=" << flitzzip::hip_runtime_version_text(info.runtime_version) << "\n";
    std::cout << "hip_stream_ordered_allocator_supported="
              << (info.stream_ordered_allocator_supported ? "true" : "false") << "\n";
    std::cout << "available=" << (info.available ? "true" : "false") << "\n";
    std::cout << "device_count=" << info.device_count << "\n";
    std::cout << "selected_device=" << info.selected_device << "\n";
    std::cout << "vram_total_bytes=" << info.vram_total_bytes << "\n";
    std::cout << "vram_free_bytes=" << info.vram_free_bytes << "\n";
    std::cout << "device_name=" << info.device_name << "\n";
    std::cout << "gcn_arch=" << info.gcn_arch << "\n";
    std::cout << "status=" << info.status << "\n";
}

// Purpose: Print HIP-only diagnostic results in a stable machine-readable format.
// Inputs: `result` is the completed diagnostic output.
// Outputs: Writes one key/value block to stdout.
void print_gpu_diagnostic(const flitzzip::GpuDiagnosticResult& result) {
    std::cout << "hip_compiled=" << (result.info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (result.info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << result.info.runtime_name << "\n";
    std::cout << "hip_runtime_version=" << flitzzip::hip_runtime_version_text(result.info.runtime_version) << "\n";
    std::cout << "available=" << (result.info.available ? "true" : "false") << "\n";
    std::cout << "device_name=" << result.info.device_name << "\n";
    std::cout << "gcn_arch=" << result.info.gcn_arch << "\n";
    std::cout << "diagnostic_bytes=" << result.bytes << "\n";
    std::cout << "diagnostic_kernel_launches=" << result.kernel_launches << "\n";
    std::cout << "diagnostic_kernel_ms=" << result.kernel_ms << "\n";
    std::cout << "diagnostic_h2d_bytes=" << result.h2d_bytes << "\n";
    std::cout << "diagnostic_d2h_bytes=" << result.d2h_bytes << "\n";
    std::cout << "diagnostic_device_allocation_bytes=" << result.device_allocation_bytes << "\n";
    std::cout << "diagnostic_checksum=" << result.checksum << "\n";
    std::cout << "diagnostic_wall_seconds=" << result.wall_seconds << "\n";
}

// Purpose: Convert AMD HIP dependency status into deterministic installer-friendly process codes.
// Inputs: `info` is the GPU/runtime status returned by the backend.
// Outputs: Returns 0 when a HIP build, runtime, and AMD device are available; otherwise returns a stable nonzero code.
int dependency_exit_code(const flitzzip::GpuInfo& info) {
    if (!info.hip_compiled) {
        return 10;
    }
    if (!info.hip_runtime_loadable) {
        return 11;
    }
    if (!info.available) {
        return info.device_count != 0 ? 13 : 12;
    }
    return 0;
}

// Purpose: Read the value following a named command-line flag.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag
// label for diagnostics. Outputs: Returns the following argument value; throws `ArchiveError` when the value is
// missing.
std::string require_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    if (index + 1 >= args.size()) {
        throw flitzzip::ArchiveError(std::string("missing value for ") + name);
    }
    ++index;
    return args[index];
}

// Purpose: Parse an unsigned 32-bit tuning argument.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag
// label for diagnostics. Outputs: Returns the parsed integer; throws `ArchiveError` when the value is invalid.
std::uint32_t require_u32_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    const auto text = require_arg(args, index, name);
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > std::numeric_limits<std::uint32_t>::max()) {
        throw flitzzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
    }
    return static_cast<std::uint32_t>(value);
}

// Purpose: Parse a bounded signed integer tuning argument.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, `name` labels
// diagnostics, and `minimum`/`maximum` define the accepted range. Outputs: Returns the parsed integer; throws
// `ArchiveError` when the value is invalid or out of range.
int require_int_arg(const std::vector<std::string>& args, std::size_t& index, const char* name, int minimum,
                    int maximum) {
    const auto text = require_arg(args, index, name);
    int value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum) {
        throw flitzzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
    }
    return value;
}

// Purpose: Parse a supported FLZIP block size in KiB for compression and benchmarking.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` labels
// diagnostics. Outputs: Returns a production-supported block size in bytes; throws `ArchiveError` for unsupported
// values.
std::uint32_t require_block_size_kib_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    const auto kib = require_u32_arg(args, index, name);
    switch (kib) {
    case 256U:
    case 512U:
    case 1024U:
    case 2048U:
    case 4096U:
    case 8192U:
    case 16384U:
        return kib * 1024U;
    default:
        throw flitzzip::ArchiveError("unsupported block size; use 256, 512, 1024, 2048, 4096, 8192, or 16384 KiB");
    }
}

struct CliCompressCommand {
    std::string format = "flzip";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    std::uint32_t block_size = flitzzip::kDefaultArchiveBlockBytes;
    bool flzip_tuning_requested = false;
    int compression_level = flitzzip::kDefaultCompressionLevel;
    bool compression_level_requested = false;
    bool neutron_star = false;
    bool verify_after_write = false;
    bool sha256 = false;
    bool defender_scan = false;
    std::filesystem::path output;
    std::vector<std::filesystem::path> sources;
};

struct CliExtractCommand {
    std::string format = "auto";
    flitzzip::ArchivePathEncoding name_encoding = flitzzip::ArchivePathEncoding::Utf8;
    bool name_encoding_requested = false;
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    bool flzip_tuning_requested = false;
    bool overwrite = false;
    bool sha256 = false;
    bool defender_scan = false;
    bool validate_before_publish = false;
    std::filesystem::path output;
    std::filesystem::path archive;
};

struct CliVerifyCommand {
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    bool flzip_tuning_requested = false;
    bool sha256 = false;
    bool defender_scan = false;
    std::filesystem::path archive;
};

// Purpose: Reject FLZIP-only tuning flags for compatibility create backends.
// Inputs: `label` names the compatibility backend and the booleans are parsed command flags.
// Outputs: Returns normally when no FLZIP-only flags were used; throws `ArchiveError` otherwise.
void reject_compat_create_tuning(std::string_view label, bool require_gpu, bool force_cpu,
                                 bool flzip_tuning_requested) {
    if (require_gpu || force_cpu || flzip_tuning_requested) {
        throw flitzzip::ArchiveError(std::string(label) + " compatibility does not support FLZIP GPU, worker, "
                                                          "or block-size flags");
    }
}

// Purpose: Reject a compression-level request for stored or fixed-level create backends.
// Inputs: `label` names the backend and `compression_level_requested` records whether the flag was passed.
// Outputs: Returns normally when no level was requested; throws `ArchiveError` otherwise.
void reject_compat_compression_level(std::string_view label, bool compression_level_requested) {
    if (compression_level_requested) {
        throw flitzzip::ArchiveError(std::string(label) + " compatibility does not support compression-level flags");
    }
}

// Purpose: Reject FLZIP-only tuning flags for compatibility extract backends.
// Inputs: `label` names the compatibility backend and the booleans are parsed command flags.
// Outputs: Returns normally when no FLZIP-only flags were used; throws `ArchiveError` otherwise.
void reject_compat_extract_tuning(std::string_view label, bool require_gpu, bool force_cpu,
                                  bool flzip_tuning_requested) {
    if (require_gpu || force_cpu || flzip_tuning_requested) {
        throw flitzzip::ArchiveError(std::string(label) +
                                     " compatibility does not support FLZIP GPU, worker, or in-flight flags");
    }
}

// Purpose: Parse `compress` command options without running filesystem operations.
// Inputs: `args` is the complete CLI argument vector whose first item is `compress`.
// Outputs: Returns parsed compression command data; throws `ArchiveError` for unknown or invalid flags.
CliCompressCommand parse_compress_command(const std::vector<std::string>& args) {
    CliCompressCommand command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--format") {
            command.format = require_arg(args, i, "--format");
        } else if (args[i] == "--output") {
            command.output = cli_path_argument(require_arg(args, i, "--output"));
        } else if (args[i] == "--require-gpu") {
            command.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            command.force_cpu = true;
        } else if (args[i] == "--workers") {
            command.workers = require_u32_arg(args, i, "--workers");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--block-size-kib") {
            command.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--compression-level") {
            command.compression_level = require_int_arg(args, i, "--compression-level", flitzzip::kMinCompressionLevel,
                                                        flitzzip::kMaxCompressionLevel);
            command.compression_level_requested = true;
        } else if (args[i] == "--verify-after-write") {
            command.verify_after_write = true;
        } else if (args[i] == "--neutron-star") {
            command.neutron_star = true;
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else {
            command.sources.push_back(cli_path_argument(args[i]));
        }
    }
    if (command.neutron_star) {
        if (command.force_cpu ||
            (command.compression_level_requested && command.compression_level != flitzzip::kMaxCompressionLevel)) {
            throw flitzzip::ArchiveError("--neutron-star excludes --force-cpu and requires compression level 9");
        }
        command.require_gpu = true;
        command.compression_level = flitzzip::kMaxCompressionLevel;
    }
    return command;
}

// Purpose: Run the selected create backend after command-line validation.
// Inputs: `archive_format` is concrete and `command` contains parsed compression options, including optional read-back.
// Outputs: Returns operation statistics; throws for unsupported formats or backend errors.
flitzzip::OperationStats compress_by_format(flitzzip::ArchiveFormat archive_format, const CliCompressCommand& command) {
    if (command.neutron_star && archive_format != flitzzip::ArchiveFormat::FlitzZip) {
        throw flitzzip::ArchiveError("Neutron star mode is available only for native FLZIP creation");
    }
    if (command.verify_after_write && archive_format != flitzzip::ArchiveFormat::FlitzZip &&
        archive_format != flitzzip::ArchiveFormat::Zip) {
        throw flitzzip::ArchiveError("verify-after-write is currently supported only for FLZIP and ZIP");
    }
    switch (archive_format) {
    case flitzzip::ArchiveFormat::FlitzZip: {
        flitzzip::CompressOptions options;
        options.gpu_required = command.require_gpu;
        options.force_cpu = command.force_cpu;
        options.worker_count = command.workers;
        options.max_inflight_chunks = command.inflight;
        options.block_size = command.block_size;
        options.compression_level = command.compression_level;
        options.compression_mode = command.neutron_star ? flitzzip::NativeCompressionMode::NeutronStar
                                                        : flitzzip::NativeCompressionMode::Standard;
        options.verify_after_write = command.verify_after_write;
        return flitzzip::compress_flzip(command.sources, command.output, options);
    }
    case flitzzip::ArchiveFormat::Zip:
        reject_compat_create_tuning("ZIP", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_zip(command.sources, command.output, command.compression_level, {},
                                      command.verify_after_write);
    case flitzzip::ArchiveFormat::Tar:
        reject_compat_create_tuning("TAR", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        reject_compat_compression_level("TAR", command.compression_level_requested);
        return flitzzip::compress_tar(command.sources, command.output);
    case flitzzip::ArchiveFormat::TarGzip:
        reject_compat_create_tuning("TAR.GZ", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_tar_gzip(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::TarBzip2:
        reject_compat_create_tuning("TAR.BZ2", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_tar_bzip2(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::TarZstd:
        reject_compat_create_tuning("TAR.ZST", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_tar_zstd(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::Gzip:
        reject_compat_create_tuning("Gzip", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_gzip(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::Bzip2:
        reject_compat_create_tuning("Bzip2", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_bzip2(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::Zstd:
        reject_compat_create_tuning("Zstandard", command.require_gpu, command.force_cpu,
                                    command.flzip_tuning_requested);
        return flitzzip::compress_zstd(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::UnixCompress:
        reject_compat_create_tuning("Unix Compress", command.require_gpu, command.force_cpu,
                                    command.flzip_tuning_requested);
        reject_compat_compression_level("Unix Compress", command.compression_level_requested);
        return flitzzip::compress_unix_compress(command.sources, command.output);
    case flitzzip::ArchiveFormat::Cpio:
        reject_compat_create_tuning("CPIO", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        reject_compat_compression_level("CPIO", command.compression_level_requested);
        return flitzzip::compress_cpio(command.sources, command.output);
    case flitzzip::ArchiveFormat::CpioGzip:
        reject_compat_create_tuning("CPIO.GZ", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        return flitzzip::compress_cpio_gzip(command.sources, command.output, command.compression_level);
    case flitzzip::ArchiveFormat::Ar:
        reject_compat_create_tuning("AR", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        reject_compat_compression_level("AR", command.compression_level_requested);
        return flitzzip::compress_ar(command.sources, command.output);
    default:
        throw flitzzip::ArchiveError(std::string("archive format recognized but not implemented for compression: ") +
                                     flitzzip::archive_format_info(archive_format).key);
    }
}

// Purpose: Execute the `compress` CLI command.
// Inputs: `args` is the full argument vector beginning with `compress`.
// Outputs: Returns a process exit code, fails closed on an enabled Defender scan, and writes successful operation
// telemetry to stdout.
int run_compress_command(const std::vector<std::string>& args) {
    const auto command = parse_compress_command(args);
    if (command.output.empty() || command.sources.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw flitzzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    const auto archive_format = resolve_cli_archive_format(command.format, command.output, false);
    reject_unsupported_cli_format(archive_format, "create");
    const auto stats = compress_by_format(archive_format, command);
    if (command.defender_scan) {
        print_defender_scan(command.output, true);
    }
    print_stats(stats);
    if (command.sha256) {
        print_integrity_hash(command.output);
    }
    return 0;
}

// Purpose: Parse `extract` command options without running filesystem operations.
// Inputs: `args` is the complete CLI argument vector whose first item is `extract`.
// Outputs: Returns parsed extraction command data; throws `ArchiveError` for unknown or invalid flags.
CliExtractCommand parse_extract_command(const std::vector<std::string>& args) {
    CliExtractCommand command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--format") {
            command.format = require_arg(args, i, "--format");
        } else if (args[i] == "--name-encoding") {
            command.name_encoding = flitzzip::parse_archive_name_encoding(require_arg(args, i, "--name-encoding"));
            command.name_encoding_requested = true;
        } else if (args[i] == "--output") {
            command.output = cli_path_argument(require_arg(args, i, "--output"));
        } else if (args[i] == "--require-gpu") {
            command.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            command.force_cpu = true;
        } else if (args[i] == "--workers") {
            command.workers = require_u32_arg(args, i, "--workers");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--overwrite") {
            command.overwrite = true;
        } else if (args[i] == "--validate-before-publish") {
            command.validate_before_publish = true;
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else {
            if (args[i].starts_with("--")) {
                throw flitzzip::ArchiveError("unknown extract argument: " + args[i]);
            }
            if (!command.archive.empty()) {
                throw flitzzip::ArchiveError("extract accepts exactly one archive");
            }
            command.archive = cli_path_argument(args[i]);
        }
    }
    return command;
}

// Purpose: Reject FLZIP-only extraction tuning on a compatibility backend.
// Inputs: `label` names the backend family and `command` carries parsed GPU/tuning flags.
// Outputs: Returns normally when compatibility extraction is untuned; throws `ArchiveError` otherwise.
void reject_extract_tuning(std::string_view label, const CliExtractCommand& command) {
    reject_compat_extract_tuning(label, command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
}

// Purpose: Run native FLZIP extraction from parsed CLI options.
// Inputs: `command` contains archive path, output directory, overwrite policy, and FLZIP tuning flags.
// Outputs: Returns operation statistics from `extract_flzip`.
flitzzip::OperationStats extract_native_flzip(const CliExtractCommand& command) {
    flitzzip::ExtractOptions options;
    options.gpu_required = command.require_gpu;
    options.force_cpu = command.force_cpu;
    options.overwrite = command.overwrite;
    options.worker_count = command.workers;
    options.max_inflight_chunks = command.inflight;
    return flitzzip::extract_flzip(command.archive, command.output, options);
}

// Purpose: Run ZIP, TAR, and single-stream compatibility extraction routes.
// Inputs: `archive_format` is concrete and `command` contains paths plus overwrite policy.
// Outputs: Returns operation statistics when the format belongs to this group; otherwise returns empty.
std::optional<flitzzip::OperationStats> extract_stream_or_tar_format(flitzzip::ArchiveFormat archive_format,
                                                                     const CliExtractCommand& command) {
    switch (archive_format) {
    case flitzzip::ArchiveFormat::Zip:
    case flitzzip::ArchiveFormat::Zipx:
        reject_extract_tuning("ZIP/ZIPX", command);
        return flitzzip::extract_zip(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::SevenZip:
        reject_extract_tuning("7z", command);
        return flitzzip::extract_7z(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Tar:
        reject_extract_tuning("TAR", command);
        return flitzzip::extract_tar(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::TarGzip:
        reject_extract_tuning("TAR.GZ", command);
        return flitzzip::extract_tar_gzip(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::TarBzip2:
        reject_extract_tuning("TAR.BZ2", command);
        return flitzzip::extract_tar_bzip2(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::TarXz:
        reject_extract_tuning("TAR.XZ", command);
        return flitzzip::extract_tar_xz(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::TarLzip:
        reject_extract_tuning("TAR.LZ", command);
        return flitzzip::extract_tar_lzip(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::TarZstd:
        reject_extract_tuning("TAR.ZST", command);
        return flitzzip::extract_tar_zstd(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Gzip:
        reject_extract_tuning("Gzip", command);
        return flitzzip::extract_gzip_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Bzip2:
        reject_extract_tuning("Bzip2", command);
        return flitzzip::extract_bzip2_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Xz:
        reject_extract_tuning("XZ", command);
        return flitzzip::extract_xz_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Lzma:
        reject_extract_tuning("LZMA", command);
        return flitzzip::extract_lzma_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Lzip:
        reject_extract_tuning("lzip", command);
        return flitzzip::extract_lzip_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Zstd:
        reject_extract_tuning("Zstandard", command);
        return flitzzip::extract_zstd_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::UnixCompress:
        reject_extract_tuning("Unix Compress", command);
        return flitzzip::extract_unix_compress_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Base64:
        reject_extract_tuning("Base64", command);
        return flitzzip::extract_base64_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Hqx:
        reject_extract_tuning("HQX", command);
        return flitzzip::extract_hqx_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::MacBinary:
        reject_extract_tuning("MacBinary", command);
        return flitzzip::extract_macbinary_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Xxe:
        reject_extract_tuning("XXE", command);
        return flitzzip::extract_xxe_file(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Uue:
        reject_extract_tuning("UUE", command);
        return flitzzip::extract_uue_file(command.archive, command.output, command.overwrite);
    default:
        return std::nullopt;
    }
}

// Purpose: Run package/container compatibility extraction routes.
// Inputs: `archive_format` is concrete and `command` contains paths plus overwrite policy.
// Outputs: Returns operation statistics when the format belongs to this group; otherwise returns empty.
std::optional<flitzzip::OperationStats> extract_container_format(flitzzip::ArchiveFormat archive_format,
                                                                 const CliExtractCommand& command) {
    switch (archive_format) {
    case flitzzip::ArchiveFormat::Cab:
        reject_extract_tuning("CAB", command);
        return flitzzip::extract_cab(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Iso:
        reject_extract_tuning("ISO", command);
        return flitzzip::extract_iso(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Cpio:
        reject_extract_tuning("CPIO", command);
        return flitzzip::extract_cpio(command.archive, command.output, command.overwrite, {}, command.name_encoding);
    case flitzzip::ArchiveFormat::CpioGzip:
        reject_extract_tuning("CPIO.GZ", command);
        return flitzzip::extract_cpio_gzip(command.archive, command.output, command.overwrite, {},
                                           command.name_encoding);
    case flitzzip::ArchiveFormat::Ar:
    case flitzzip::ArchiveFormat::Deb:
        reject_extract_tuning("AR/DEB", command);
        return flitzzip::extract_ar(command.archive, command.output, command.overwrite, {}, command.name_encoding);
    case flitzzip::ArchiveFormat::Arj:
        reject_extract_tuning("ARJ", command);
        return flitzzip::extract_arj(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Arc:
        reject_extract_tuning("ARC", command);
        return flitzzip::extract_arc(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Rpm:
        reject_extract_tuning("RPM", command);
        return flitzzip::extract_rpm(command.archive, command.output, command.overwrite, {}, command.name_encoding);
    case flitzzip::ArchiveFormat::Lha:
        reject_extract_tuning("LHA", command);
        return flitzzip::extract_lha(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Wim:
        reject_extract_tuning("WIM", command);
        return flitzzip::extract_wim(command.archive, command.output, command.overwrite);
    case flitzzip::ArchiveFormat::Xar:
        reject_extract_tuning("XAR", command);
        return flitzzip::extract_xar(command.archive, command.output, command.overwrite);
    default:
        return std::nullopt;
    }
}

// Purpose: Run the selected extract backend after command-line validation.
// Inputs: `archive_format` is concrete and `command` contains parsed extraction options.
// Outputs: Returns operation statistics; throws for unsupported formats or backend errors.
flitzzip::OperationStats extract_by_format(flitzzip::ArchiveFormat archive_format, const CliExtractCommand& command) {
    if (archive_format == flitzzip::ArchiveFormat::FlitzZip) {
        return extract_native_flzip(command);
    }
    if (auto result = extract_stream_or_tar_format(archive_format, command); result.has_value()) {
        return *result;
    }
    if (auto result = extract_container_format(archive_format, command); result.has_value()) {
        return *result;
    }
    throw flitzzip::ArchiveError(std::string("archive format recognized but not implemented for extraction: ") +
                                 flitzzip::archive_format_info(archive_format).key);
}

// Purpose: Execute the `extract` CLI command.
// Inputs: `args` is the full argument vector beginning with `extract`.
// Outputs: Extracts once with the shared publication policy, writes telemetry, and returns a process exit code.
int run_extract_command(const std::vector<std::string>& args) {
    auto command = parse_extract_command(args);
    if (command.output.empty() || command.archive.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw flitzzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    const auto archive_source = flitzzip::pin_source_file(command.archive);
    command.archive = archive_source.path();
    if (command.sha256) {
        print_integrity_hash(archive_source.path());
    }
    if (command.defender_scan) {
        print_defender_scan(archive_source.path(), true);
    }
    const auto archive_format = resolve_cli_archive_format(command.format, archive_source.path(), true);
    reject_unsupported_cli_format(archive_format, "extract");
    if (command.name_encoding_requested && !flitzzip::archive_format_info(archive_format).supports_name_encoding) {
        throw flitzzip::ArchiveError("--name-encoding is supported only for CPIO, CPIO.GZ, AR, DEB, and RPM");
    }
    flitzzip::OperationStats stats;
    const auto extraction_start = std::chrono::steady_clock::now();
    std::function<void(const std::filesystem::path&)> inspect_output;
    if (command.defender_scan)
        inspect_output = [](const auto& directory) { print_defender_scan(directory, true); };
    flitzzip::extract_with_publication(
        {.destination = command.output,
         .overwrite = command.overwrite,
         .validate_before_publish = command.validate_before_publish},
        [&](const auto& directory, bool overwrite) {
            auto extraction = command;
            extraction.output = directory;
            extraction.overwrite = overwrite;
            stats = extract_by_format(archive_format, extraction);
        },
        inspect_output);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - extraction_start).count();
    print_stats(stats);
    if (command.sha256) {
        print_integrity_hash(command.output, "output_integrity");
    }
    return 0;
}

// Purpose: Inspect corpus extent without allocating or authenticating its payload.
// Inputs: User-selected regular file; the CLI's 32-bit extent representation applies before memory admission.
// Outputs: Returns checked metadata bytes or throws; this does not verify the source SHA-256.
std::size_t memory_benchmark_corpus_size(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        throw flitzzip::ArchiveError("benchmark source must be a regular file");
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (!input || length <= 0 || length > static_cast<std::streamoff>(flitzzip::cli::kMemoryBenchmarkCorpusMaxBytes)) {
        throw flitzzip::ArchiveError("benchmark corpus must contain 1..4294967295 bytes within current host headroom");
    }
    return static_cast<std::size_t>(length);
}

// Purpose: Preload a bounded corpus snapshot before any measured benchmark phase.
// Inputs: User-selected file and its checked metadata extent; host headroom applies before allocation.
// Outputs: Owns exact bytes or throws on invalid size/read; no payload is written to storage.
std::vector<std::byte> load_memory_benchmark_corpus(const std::filesystem::path& path, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    return flitzzip::cli::load_memory_benchmark_stream(input, size);
}

struct MemoryBenchmarkSourceArguments {
    std::optional<std::filesystem::path> file;
    bool stdin_requested = false;
    std::optional<std::uint32_t> bytes;
};

// Purpose: Parse exact RAM-source declarations independently of codec and generated-workload options.
// Inputs: Full argument vector, current cursor and command-local source/options state.
// Outputs: Consumes a recognized source option; rejects duplicate declarations; leaves other options untouched.
bool parse_memory_benchmark_source_argument(const std::vector<std::string>& args, std::size_t& i,
                                            MemoryBenchmarkSourceArguments& source,
                                            flitzzip::cli::MemoryBenchmarkOptions& options) {
    if (args[i] == "--source-file") {
        if (source.file) {
            throw flitzzip::ArchiveError("duplicate --source-file");
        }
        const auto value = require_arg(args, i, "--source-file");
        source.file = std::filesystem::path(std::u8string(value.begin(), value.end()));
    } else if (args[i] == "--source-stdin") {
        if (source.stdin_requested) {
            throw flitzzip::ArchiveError("duplicate --source-stdin");
        }
        source.stdin_requested = true;
    } else if (args[i] == "--source-bytes") {
        if (source.bytes) {
            throw flitzzip::ArchiveError("duplicate --source-bytes");
        }
        source.bytes = require_u32_arg(args, i, "--source-bytes");
    } else if (args[i] == "--source-sha256") {
        options.expected_source_sha256 = require_arg(args, i, "--source-sha256");
    } else {
        return false;
    }
    return true;
}

// Purpose: Execute `memory-benchmark` after parsing generated or exact corpus options.
// Inputs: `args` is the full argument vector beginning with `memory-benchmark`.
// Outputs: Returns measured telemetry or a generated allocation-free plan; throws on invalid identity/admission.
int run_memory_benchmark_command(const std::vector<std::string>& args) {
    flitzzip::cli::MemoryBenchmarkOptions options;
    bool plan_only = false;
    bool level_requested = false;
    bool generated_geometry = false;
    MemoryBenchmarkSourceArguments source_arguments;
    // Parsing: source declarations are separate from generated geometry and codec policy.
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (parse_memory_benchmark_source_argument(args, i, source_arguments, options)) {
            continue;
        } else if (args[i] == "--size-mib") {
            generated_geometry = true;
            options.size_mib = require_u32_arg(args, i, "--size-mib");
        } else if (args[i] == "--profile") {
            generated_geometry = true;
            options.profile = require_arg(args, i, "--profile");
        } else if (args[i] == "--require-gpu") {
            options.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            options.force_cpu = true;
        } else if (args[i] == "--workers") {
            options.workers = require_u32_arg(args, i, "--workers");
        } else if (args[i] == "--inflight") {
            options.inflight_chunks = require_u32_arg(args, i, "--inflight");
        } else if (args[i] == "--decode-inflight") {
            options.decode_inflight_chunks = require_u32_arg(args, i, "--decode-inflight");
        } else if (args[i] == "--plan-only") {
            plan_only = true;
        } else if (args[i] == "--block-size-kib") {
            options.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
        } else if (args[i] == "--compression-level") {
            options.compression_level = require_int_arg(args, i, "--compression-level", flitzzip::kMinCompressionLevel,
                                                        flitzzip::kMaxCompressionLevel);
            level_requested = true;
        } else if (args[i] == "--neutron-star") {
            options.compression_mode = flitzzip::NativeCompressionMode::NeutronStar;
        } else {
            throw flitzzip::ArchiveError("unknown memory-benchmark argument: " + args[i]);
        }
    }
    if (options.compression_mode == flitzzip::NativeCompressionMode::NeutronStar) {
        if (options.force_cpu || (level_requested && options.compression_level != flitzzip::kMaxCompressionLevel)) {
            throw flitzzip::ArchiveError("--neutron-star excludes --force-cpu and requires compression level 9");
        }
        options.require_gpu = true;
        options.compression_level = flitzzip::kMaxCompressionLevel;
    }
    // Ownership: the snapshot outlives every codec and bytewise-readback worker.
    std::vector<std::byte> source;
    if (source_arguments.stdin_requested) {
        if (source_arguments.file || generated_geometry || !source_arguments.bytes ||
            options.expected_source_sha256.empty()) {
            throw flitzzip::ArchiveError(
                "--source-stdin requires --source-bytes and --source-sha256; excludes file/generated input");
        }
        if (*source_arguments.bytes == 0U) {
            throw flitzzip::ArchiveError(
                "benchmark corpus must contain 1..4294967295 bytes within current host headroom");
        }
        options.corpus_bytes = *source_arguments.bytes;
        options.profile = "Corpus";
        if (!plan_only) {
            (void)flitzzip::cli::plan_memory_benchmark(options);
            // Binary mode preserves CRLF, NUL and Ctrl-Z in this dedicated CLI process.
            if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
                throw flitzzip::ArchiveError("could not select binary benchmark stdin");
            }
            source = flitzzip::cli::load_memory_benchmark_stream(std::cin, *source_arguments.bytes);
            options.source = source;
        }
    } else if (source_arguments.bytes) {
        throw flitzzip::ArchiveError("--source-bytes requires --source-stdin");
    }
    if (source_arguments.file) {
        if (generated_geometry || options.expected_source_sha256.empty()) {
            throw flitzzip::ArchiveError(
                "--source-file requires --source-sha256 and excludes --size-mib and --profile");
        }
        options.corpus_bytes = memory_benchmark_corpus_size(*source_arguments.file);
        options.profile = "Corpus";
        if (!plan_only) {
            (void)flitzzip::cli::plan_memory_benchmark(options);
            source =
                load_memory_benchmark_corpus(*source_arguments.file, static_cast<std::size_t>(options.corpus_bytes));
            options.source = source;
        }
        options.profile = "Corpus";
    }
    if (plan_only) {
        flitzzip::cli::print_memory_benchmark_plan(flitzzip::cli::plan_memory_benchmark(options));
    } else {
        flitzzip::cli::print_memory_benchmark_stats(flitzzip::cli::run_memory_benchmark(options));
    }
    return 0;
}

// Purpose: Execute `benchmark-suite` after parsing bounded options.
// Inputs: `args` is the full argument vector beginning with `benchmark-suite`.
// Outputs: Returns zero and prints benchmark-suite telemetry.
int run_benchmark_suite_command(const std::vector<std::string>& args) {
    flitzzip::cli::BenchmarkSuiteOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--size-mib") {
            options.size_mib = require_u32_arg(args, i, "--size-mib");
        } else if (args[i] == "--profile") {
            options.profile = require_arg(args, i, "--profile");
        } else if (args[i] == "--workers") {
            options.workers = require_u32_arg(args, i, "--workers");
        } else if (args[i] == "--block-size-kib") {
            options.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
        } else if (args[i] == "--compression-level") {
            options.compression_level = require_int_arg(args, i, "--compression-level", flitzzip::kMinCompressionLevel,
                                                        flitzzip::kMaxCompressionLevel);
        } else if (args[i] == "--tune") {
            options.tune = true;
        } else if (args[i] == "--tune-levels") {
            options.tune = true;
            options.tune_levels = true;
        } else {
            throw flitzzip::ArchiveError("unknown benchmark-suite argument: " + args[i]);
        }
    }
    flitzzip::cli::run_benchmark_suite(options);
    return 0;
}

// Purpose: Parse `verify` command options without running filesystem operations.
// Inputs: `args` is the complete CLI argument vector whose first item is `verify`.
// Outputs: Returns parsed verification command data; throws `ArchiveError` for unknown or invalid flags.
CliVerifyCommand parse_verify_command(const std::vector<std::string>& args) {
    CliVerifyCommand command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--require-gpu") {
            command.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            command.force_cpu = true;
        } else if (args[i] == "--workers") {
            command.workers = require_u32_arg(args, i, "--workers");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
            command.flzip_tuning_requested = true;
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else if (args[i].starts_with("--")) {
            throw flitzzip::ArchiveError("unknown verify argument: " + args[i]);
        } else {
            if (!command.archive.empty()) {
                throw flitzzip::ArchiveError("verify accepts exactly one archive");
            }
            command.archive = cli_path_argument(args[i]);
        }
    }
    return command;
}

// Purpose: Execute read-only verification for native FLZIP and supported ZIP archives.
// Inputs: `args` is the full argument vector beginning with `verify`; GPU/worker flags apply only to FLZIP.
// Outputs: Returns an exit code and telemetry, or throws on unsupported formats, flags, or archive validation errors.
int run_verify_command(const std::vector<std::string>& args) {
    const auto command = parse_verify_command(args);
    if (command.archive.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw flitzzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    const auto format = flitzzip::detect_archive_format(command.archive);
    if (format == flitzzip::ArchiveFormat::Zip || format == flitzzip::ArchiveFormat::Zipx) {
        reject_compat_extract_tuning("ZIP", command.require_gpu, command.force_cpu, command.flzip_tuning_requested);
        print_stats(flitzzip::verify_zip(command.archive));
    } else if (format == flitzzip::ArchiveFormat::FlitzZip) {
        flitzzip::ExtractOptions options;
        options.gpu_required = command.require_gpu;
        options.force_cpu = command.force_cpu;
        options.worker_count = command.workers;
        options.max_inflight_chunks = command.inflight;
        print_stats(flitzzip::verify_flzip(command.archive, options));
    } else {
        throw flitzzip::ArchiveError("read-only verify is currently supported only for FLZIP and ZIP");
    }
    if (command.sha256) {
        print_integrity_hash(command.archive);
    }
    if (command.defender_scan) {
        print_defender_scan(command.archive, true);
    }
    return 0;
}

// Purpose: Execute non-archive informational CLI commands.
// Inputs: `args` is the full argument vector beginning with an informational command.
// Outputs: Returns the command exit code or `std::nullopt` when `args[0]` is not informational.
std::optional<int> run_info_command(const std::vector<std::string>& args) {
    if (args[0] == "--help" || args[0] == "--version") {
        if (args.size() != 1) {
            usage();
            return 2;
        }
        if (args[0] == "--help") {
            usage();
        } else {
            std::cout << "FlitzZip " << FLITZZIP_VERSION << "\n";
        }
        return 0;
    }
    if (args[0] == "gpu-info") {
        const auto info = flitzzip::query_gpu_info();
        print_gpu_info(info);
        return info.available ? 0 : 1;
    }
    if (args[0] == "dependency-check") {
        const auto info = flitzzip::query_gpu_info();
        print_gpu_info(info);
        const int code = dependency_exit_code(info);
        std::cout << "dependency_status="
                  << (code == 0    ? "ready"
                      : code == 10 ? "cpu_only_build"
                      : code == 11 ? "missing_hip_runtime"
                      : code == 12 ? "missing_amd_gpu"
                                   : "hip_initialization_or_kernel_failure")
                  << "\n";
        return code;
    }
    if (args[0] == "formats") {
        if (args.size() != 1) {
            usage();
            return 2;
        }
        print_format_registry();
        return 0;
    }
    if (args[0] == "identify") {
        if (args.size() != 2) {
            usage();
            return 2;
        }
        const auto format = resolve_cli_archive_format("auto", cli_path_argument(args[1]), true);
        const auto& info = flitzzip::archive_format_info(format);
        std::cout << "format=" << info.key << " display=\"" << info.display_name << "\""
                  << " can_create=" << (info.can_create ? "true" : "false")
                  << " can_extract=" << (info.can_extract ? "true" : "false")
                  << " gpu_native=" << (info.gpu_native ? "true" : "false")
                  << " bundled_native=" << (info.bundled_native ? "true" : "false") << "\n";
        return 0;
    }
    return std::nullopt;
}

// Purpose: Execute the GPU diagnostic CLI command.
// Inputs: `args` is the full argument vector beginning with `gpu-diagnostic`.
// Outputs: Returns zero and prints diagnostic telemetry.
int run_gpu_diagnostic_command(const std::vector<std::string>& args) {
    flitzzip::GpuDiagnosticOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--seconds") {
            const auto seconds = require_int_arg(args, i, "--seconds", 1, 30);
            options.seconds = static_cast<double>(seconds);
        } else if (args[i] == "--buffer-mib") {
            options.buffer_mib = require_u32_arg(args, i, "--buffer-mib");
        } else if (args[i] == "--inner-iterations") {
            options.inner_iterations = require_u32_arg(args, i, "--inner-iterations");
        } else {
            throw flitzzip::ArchiveError("unknown gpu-diagnostic argument: " + args[i]);
        }
    }
    print_gpu_diagnostic(flitzzip::run_gpu_diagnostic(options));
    return 0;
}

// Purpose: Route one parsed command vector to its implementation.
// Inputs: `args` is non-empty and excludes the executable path.
// Outputs: Returns the process exit code for the command.
int run_cli_command(const std::vector<std::string>& args) {
    if (const auto info_code = run_info_command(args); info_code.has_value()) {
        return *info_code;
    }
    if (args[0] == "gpu-diagnostic") {
        return run_gpu_diagnostic_command(args);
    }
    if (args[0] == "memory-benchmark") {
        return run_memory_benchmark_command(args);
    }
    if (args[0] == "benchmark-suite") {
        return run_benchmark_suite_command(args);
    }
    if (args[0] == "compress") {
        return run_compress_command(args);
    }
    if (args[0] == "extract") {
        return run_extract_command(args);
    }
    if (args[0] == "verify") {
        return run_verify_command(args);
    }
    usage();
    return 2;
}

}  // namespace

// Purpose: Execute the FlitzZip command-line interface.
// Inputs: `argc`/`argv` are process command-line arguments supplied as UTF-16 by the Windows CRT.
// Outputs: Returns 0 on success, 1 on operation failure, and 2 on invalid usage.
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            args.push_back(encode_cli_argument(argv[index]));
        }
        if (args.empty()) {
            usage();
            return 2;
        }
        return run_cli_command(args);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
