#include "cli/memory_benchmark.hpp"
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
#include "core/integrity.hpp"
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
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// Purpose: Print command-line help for supported SuperZip operations.
// Inputs: None.
// Outputs: Writes usage text to stdout.
void usage() {
    std::cout
        << "SuperZip CLI\n"
        << "Usage:\n"
        << "  superzip_cli gpu-info\n"
        << "  superzip_cli gpu-diagnostic [--seconds <1-30>] [--buffer-mib <16-512>] [--inner-iterations <1-4096>]\n"
        << "  superzip_cli dependency-check\n"
        << "  superzip_cli formats\n"
        << "  superzip_cli identify <archive>\n"
        << "  superzip_cli memory-benchmark --size-mib <n> --profile Mixed|Compressible|Incompressible "
           "[--require-gpu|--force-cpu] [--workers <n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level "
           "<1-9>]\n"
        << "  superzip_cli benchmark-suite [--size-mib <n>] [--profile Mixed|Compressible|Incompressible] [--workers "
           "<n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level <1-9>] [--tune] [--tune-levels]\n"
        << "  superzip_cli compress --format suzip --output <archive> [--require-gpu|--force-cpu] [--workers <n>] "
           "[--inflight <n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level <1-9>] "
           "[--verify-after-write] [--sha256] [--defender-scan] <path>...\n"
        << "  superzip_cli compress --format "
           "zip|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.zst|tzst|gz|gzip|bz2|bzip2|zst|zstd|z|compress|cpio|cpio.gz|"
           "cpgz|ar --output <archive> [--compression-level <1-9>] "
           "[--sha256] [--defender-scan] <path>...\n"
        << "  superzip_cli extract --format suzip --output <directory> [--require-gpu|--force-cpu] [--workers <n>] "
           "[--inflight <n>] [--overwrite] [--sha256] [--defender-scan] <archive.suzip>\n"
        << "  superzip_cli extract --format "
           "auto|zip|zipx|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.xz|txz|tar.lz|tlz|tar.zst|tzst|gz|gzip|bz2|bzip2|xz|lzma|"
           "lz|lzip|zst|zstd|z|compress|b64|base64|hqx|binhex|xxe|xxencode|uue|uu|macbinary|macbin|cab|iso|cpio|cpio."
           "gz|cpgz|ar|arj|arc|ark|deb|rpm|7z|lha|lzh|wim|swm|xar --output <directory> [--overwrite] [--sha256] "
           "[--defender-scan] <archive>\n"
        << "  superzip_cli verify [--require-gpu|--force-cpu] [--workers <n>] [--inflight <n>] [--sha256] "
           "[--defender-scan] <archive.suzip>\n";
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
// Outputs: Writes one line to stdout.
void print_stats(const superzip::OperationStats& stats) {
    std::cout << "entries=" << stats.entries << " input_bytes=" << stats.input_bytes
              << " output_bytes=" << stats.output_bytes << " workers=" << stats.workers
              << " inflight_chunks=" << stats.inflight_chunks << " gpu_used=" << (stats.gpu_used ? "true" : "false")
              << " gpu_encode_chunks=" << stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << stats.gpu_runtime.kernel_ms << " gpu_h2d_bytes=" << stats.gpu_runtime.h2d_bytes
              << " gpu_d2h_bytes=" << stats.gpu_runtime.d2h_bytes
              << " gpu_device_allocation_bytes=" << stats.gpu_runtime.device_allocation_bytes
              << " gpu_pattern_blocks=" << stats.gpu_runtime.pattern_blocks
              << " gpu_prefix_blocks=" << stats.gpu_runtime.prefix_blocks << " seconds=" << stats.seconds
              << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
              << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes) << "\n";
}

// Purpose: Print the archive formats that SuperZip recognizes and whether each is implemented.
// Inputs: None.
// Outputs: Writes one parseable line per real archive format.
void print_format_registry() {
    for (const auto& info : superzip::archive_format_registry()) {
        if (info.format == superzip::ArchiveFormat::Unknown || info.format == superzip::ArchiveFormat::Auto) {
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
superzip::ArchiveFormat resolve_cli_archive_format(const std::string& format_token,
                                                   const std::filesystem::path& archive_path, bool allow_auto) {
    const auto parsed = superzip::parse_archive_format_token(format_token);
    if (!parsed.has_value()) {
        throw superzip::ArchiveError("unknown archive format: " + format_token);
    }
    if (*parsed != superzip::ArchiveFormat::Auto) {
        return *parsed;
    }
    if (!allow_auto) {
        throw superzip::ArchiveError("archive format auto-detection is only supported for extraction and identify");
    }
    const auto detected = superzip::detect_archive_format(archive_path);
    if (detected == superzip::ArchiveFormat::Unknown) {
        throw superzip::ArchiveError("unable to detect archive format: " + archive_path.string());
    }
    return detected;
}

// Purpose: Reject recognized archive formats that SuperZip can identify but cannot yet process.
// Inputs: `format` is a concrete archive format and `operation` is `create` or `extract`.
// Outputs: Returns normally for implemented formats; throws a clear non-implementation error otherwise.
void reject_unsupported_cli_format(superzip::ArchiveFormat format, std::string_view operation) {
    const auto& info = superzip::archive_format_info(format);
    const bool supported = operation == "create" ? info.can_create : info.can_extract;
    if (!supported) {
        throw superzip::ArchiveError(std::string("archive format recognized but not yet implemented for ") +
                                     std::string(operation) + ": " + info.key);
    }
}

// Purpose: Print SHA-256 integrity data for an archive.
// Inputs: `path` is an existing file to hash.
// Outputs: Writes algorithm and digest lines to stdout; throws if hashing fails.
void print_integrity_hash(const std::filesystem::path& path) {
    const auto hash = superzip::hash_file(path, superzip::IntegrityMode::Sha256);
    std::cout << "integrity_algorithm=" << hash.algorithm << "\n";
    std::cout << "integrity_sha256=" << hash.hex_digest << "\n";
}

// Purpose: Run and print an opt-in Microsoft Defender scan result.
// Inputs: `path` is the target file or directory and `block_if_detected` controls whether a non-clean attempted scan
// aborts the operation. Outputs: Writes scan state to stdout; throws `SecurityError` when Defender reports a scanned
// target is not clean or times out and blocking is requested.
void print_defender_scan(const std::filesystem::path& path, bool block_if_detected) {
    const auto scan = superzip::scan_with_windows_defender(path, superzip::DefenderScanMode::FullPath);
    std::cout << "defender_attempted=" << (scan.attempted ? "true" : "false") << "\n";
    std::cout << "defender_clean=" << (scan.clean ? "true" : "false") << "\n";
    std::cout << "defender_timed_out=" << (scan.timed_out ? "true" : "false") << "\n";
    std::cout << "defender_exit_code=" << scan.exit_code << "\n";
    if (block_if_detected && scan.attempted && !scan.clean) {
        throw superzip::SecurityError("Microsoft Defender did not report the target as clean: " + path.string());
    }
}

// Purpose: Print AMD HIP dependency state in a stable machine-readable format.
// Inputs: `info` is the GPU/runtime status returned by the backend.
// Outputs: Writes one key/value block to stdout.
void print_gpu_info(const superzip::GpuInfo& info) {
    std::cout << "hip_compiled=" << (info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << info.runtime_name << "\n";
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
void print_gpu_diagnostic(const superzip::GpuDiagnosticResult& result) {
    std::cout << "hip_compiled=" << (result.info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (result.info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << result.info.runtime_name << "\n";
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
int dependency_exit_code(const superzip::GpuInfo& info) {
    if (!info.hip_compiled) {
        return 10;
    }
    if (!info.hip_runtime_loadable) {
        return 11;
    }
    if (!info.available) {
        return 12;
    }
    return 0;
}

// Purpose: Read the value following a named command-line flag.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag
// label for diagnostics. Outputs: Returns the following argument value; throws `ArchiveError` when the value is
// missing.
std::string require_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    if (index + 1 >= args.size()) {
        throw superzip::ArchiveError(std::string("missing value for ") + name);
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
        throw superzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
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
        throw superzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
    }
    return value;
}

// Purpose: Parse a supported SUZIP block size in KiB for compression and benchmarking.
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
        throw superzip::ArchiveError("unsupported block size; use 256, 512, 1024, 2048, 4096, 8192, or 16384 KiB");
    }
}

struct CliCompressCommand {
    std::string format = "suzip";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    bool suzip_tuning_requested = false;
    int compression_level = superzip::kDefaultCompressionLevel;
    bool compression_level_requested = false;
    bool verify_after_write = false;
    bool sha256 = false;
    bool defender_scan = false;
    std::filesystem::path output;
    std::vector<std::filesystem::path> sources;
};

struct CliExtractCommand {
    std::string format = "auto";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    bool suzip_tuning_requested = false;
    bool overwrite = false;
    bool sha256 = false;
    bool defender_scan = false;
    std::filesystem::path output;
    std::filesystem::path archive;
};

struct CliVerifyCommand {
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t inflight = 0;
    bool sha256 = false;
    bool defender_scan = false;
    std::filesystem::path archive;
};

// Purpose: Reject SUZIP-only tuning flags for compatibility create backends.
// Inputs: `label` names the compatibility backend and the booleans are parsed command flags.
// Outputs: Returns normally when no SUZIP-only flags were used; throws `ArchiveError` otherwise.
void reject_compat_create_tuning(std::string_view label, bool require_gpu, bool force_cpu,
                                 bool suzip_tuning_requested) {
    if (require_gpu || force_cpu || suzip_tuning_requested) {
        throw superzip::ArchiveError(std::string(label) + " compatibility does not support SUZIP GPU, worker, "
                                                          "block-size, or verify-after-write flags");
    }
}

// Purpose: Reject a compression-level request for stored or fixed-level create backends.
// Inputs: `label` names the backend and `compression_level_requested` records whether the flag was passed.
// Outputs: Returns normally when no level was requested; throws `ArchiveError` otherwise.
void reject_compat_compression_level(std::string_view label, bool compression_level_requested) {
    if (compression_level_requested) {
        throw superzip::ArchiveError(std::string(label) + " compatibility does not support compression-level flags");
    }
}

// Purpose: Reject SUZIP-only tuning flags for compatibility extract backends.
// Inputs: `label` names the compatibility backend and the booleans are parsed command flags.
// Outputs: Returns normally when no SUZIP-only flags were used; throws `ArchiveError` otherwise.
void reject_compat_extract_tuning(std::string_view label, bool require_gpu, bool force_cpu,
                                  bool suzip_tuning_requested) {
    if (require_gpu || force_cpu || suzip_tuning_requested) {
        throw superzip::ArchiveError(std::string(label) +
                                     " compatibility does not support SUZIP GPU, worker, or in-flight flags");
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
            command.output = require_arg(args, i, "--output");
        } else if (args[i] == "--require-gpu") {
            command.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            command.force_cpu = true;
        } else if (args[i] == "--workers") {
            command.workers = require_u32_arg(args, i, "--workers");
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--block-size-kib") {
            command.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--compression-level") {
            command.compression_level = require_int_arg(args, i, "--compression-level", superzip::kMinCompressionLevel,
                                                        superzip::kMaxCompressionLevel);
            command.compression_level_requested = true;
        } else if (args[i] == "--verify-after-write") {
            command.verify_after_write = true;
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else {
            command.sources.emplace_back(args[i]);
        }
    }
    return command;
}

// Purpose: Run the selected create backend after command-line validation.
// Inputs: `archive_format` is concrete and `command` contains parsed compression options.
// Outputs: Returns operation statistics; throws for unsupported formats or backend errors.
superzip::OperationStats compress_by_format(superzip::ArchiveFormat archive_format, const CliCompressCommand& command) {
    switch (archive_format) {
    case superzip::ArchiveFormat::SuperZip: {
        superzip::CompressOptions options;
        options.gpu_required = command.require_gpu;
        options.force_cpu = command.force_cpu;
        options.worker_count = command.workers;
        options.max_inflight_chunks = command.inflight;
        options.block_size = command.block_size;
        options.compression_level = command.compression_level;
        options.verify_after_write = command.verify_after_write;
        return superzip::compress_suzip(command.sources, command.output, options);
    }
    case superzip::ArchiveFormat::Zip:
        reject_compat_create_tuning("ZIP", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_zip(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::Tar:
        reject_compat_create_tuning("TAR", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        reject_compat_compression_level("TAR", command.compression_level_requested);
        return superzip::compress_tar(command.sources, command.output);
    case superzip::ArchiveFormat::TarGzip:
        reject_compat_create_tuning("TAR.GZ", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_tar_gzip(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::TarBzip2:
        reject_compat_create_tuning("TAR.BZ2", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_tar_bzip2(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::TarZstd:
        reject_compat_create_tuning("TAR.ZST", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_tar_zstd(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::Gzip:
        reject_compat_create_tuning("Gzip", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_gzip(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::Bzip2:
        reject_compat_create_tuning("Bzip2", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_bzip2(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::Zstd:
        reject_compat_create_tuning("Zstandard", command.require_gpu, command.force_cpu,
                                    command.suzip_tuning_requested);
        return superzip::compress_zstd(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::UnixCompress:
        reject_compat_create_tuning("Unix Compress", command.require_gpu, command.force_cpu,
                                    command.suzip_tuning_requested);
        reject_compat_compression_level("Unix Compress", command.compression_level_requested);
        return superzip::compress_unix_compress(command.sources, command.output);
    case superzip::ArchiveFormat::Cpio:
        reject_compat_create_tuning("CPIO", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        reject_compat_compression_level("CPIO", command.compression_level_requested);
        return superzip::compress_cpio(command.sources, command.output);
    case superzip::ArchiveFormat::CpioGzip:
        reject_compat_create_tuning("CPIO.GZ", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        return superzip::compress_cpio_gzip(command.sources, command.output, command.compression_level);
    case superzip::ArchiveFormat::Ar:
        reject_compat_create_tuning("AR", command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
        reject_compat_compression_level("AR", command.compression_level_requested);
        return superzip::compress_ar(command.sources, command.output);
    default:
        throw superzip::ArchiveError(std::string("archive format recognized but not implemented for compression: ") +
                                     superzip::archive_format_info(archive_format).key);
    }
}

// Purpose: Execute the `compress` CLI command.
// Inputs: `args` is the full argument vector beginning with `compress`.
// Outputs: Returns a process exit code and writes operation telemetry to stdout.
int run_compress_command(const std::vector<std::string>& args) {
    const auto command = parse_compress_command(args);
    if (command.output.empty() || command.sources.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    const auto archive_format = resolve_cli_archive_format(command.format, command.output, false);
    reject_unsupported_cli_format(archive_format, "create");
    print_stats(compress_by_format(archive_format, command));
    if (command.sha256) {
        print_integrity_hash(command.output);
    }
    if (command.defender_scan) {
        print_defender_scan(command.output, false);
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
        } else if (args[i] == "--output") {
            command.output = require_arg(args, i, "--output");
        } else if (args[i] == "--require-gpu") {
            command.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            command.force_cpu = true;
        } else if (args[i] == "--workers") {
            command.workers = require_u32_arg(args, i, "--workers");
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
            command.suzip_tuning_requested = true;
        } else if (args[i] == "--overwrite") {
            command.overwrite = true;
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else {
            command.archive = args[i];
        }
    }
    return command;
}

// Purpose: Reject SUZIP-only extraction tuning on a compatibility backend.
// Inputs: `label` names the backend family and `command` carries parsed GPU/tuning flags.
// Outputs: Returns normally when compatibility extraction is untuned; throws `ArchiveError` otherwise.
void reject_extract_tuning(std::string_view label, const CliExtractCommand& command) {
    reject_compat_extract_tuning(label, command.require_gpu, command.force_cpu, command.suzip_tuning_requested);
}

// Purpose: Run native SUZIP extraction from parsed CLI options.
// Inputs: `command` contains archive path, output directory, overwrite policy, and SUZIP tuning flags.
// Outputs: Returns operation statistics from `extract_suzip`.
superzip::OperationStats extract_native_suzip(const CliExtractCommand& command) {
    superzip::ExtractOptions options;
    options.gpu_required = command.require_gpu;
    options.force_cpu = command.force_cpu;
    options.overwrite = command.overwrite;
    options.worker_count = command.workers;
    options.max_inflight_chunks = command.inflight;
    return superzip::extract_suzip(command.archive, command.output, options);
}

// Purpose: Run ZIP, TAR, and single-stream compatibility extraction routes.
// Inputs: `archive_format` is concrete and `command` contains paths plus overwrite policy.
// Outputs: Returns operation statistics when the format belongs to this group; otherwise returns empty.
std::optional<superzip::OperationStats> extract_stream_or_tar_format(superzip::ArchiveFormat archive_format,
                                                                     const CliExtractCommand& command) {
    switch (archive_format) {
    case superzip::ArchiveFormat::Zip:
    case superzip::ArchiveFormat::Zipx:
        reject_extract_tuning("ZIP/ZIPX", command);
        return superzip::extract_zip(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::SevenZip:
        reject_extract_tuning("7z", command);
        return superzip::extract_7z(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Tar:
        reject_extract_tuning("TAR", command);
        return superzip::extract_tar(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::TarGzip:
        reject_extract_tuning("TAR.GZ", command);
        return superzip::extract_tar_gzip(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::TarBzip2:
        reject_extract_tuning("TAR.BZ2", command);
        return superzip::extract_tar_bzip2(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::TarXz:
        reject_extract_tuning("TAR.XZ", command);
        return superzip::extract_tar_xz(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::TarLzip:
        reject_extract_tuning("TAR.LZ", command);
        return superzip::extract_tar_lzip(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::TarZstd:
        reject_extract_tuning("TAR.ZST", command);
        return superzip::extract_tar_zstd(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Gzip:
        reject_extract_tuning("Gzip", command);
        return superzip::extract_gzip_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Bzip2:
        reject_extract_tuning("Bzip2", command);
        return superzip::extract_bzip2_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Xz:
        reject_extract_tuning("XZ", command);
        return superzip::extract_xz_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Lzma:
        reject_extract_tuning("LZMA", command);
        return superzip::extract_lzma_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Lzip:
        reject_extract_tuning("lzip", command);
        return superzip::extract_lzip_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Zstd:
        reject_extract_tuning("Zstandard", command);
        return superzip::extract_zstd_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::UnixCompress:
        reject_extract_tuning("Unix Compress", command);
        return superzip::extract_unix_compress_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Base64:
        reject_extract_tuning("Base64", command);
        return superzip::extract_base64_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Hqx:
        reject_extract_tuning("HQX", command);
        return superzip::extract_hqx_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::MacBinary:
        reject_extract_tuning("MacBinary", command);
        return superzip::extract_macbinary_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Xxe:
        reject_extract_tuning("XXE", command);
        return superzip::extract_xxe_file(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Uue:
        reject_extract_tuning("UUE", command);
        return superzip::extract_uue_file(command.archive, command.output, command.overwrite);
    default:
        return std::nullopt;
    }
}

// Purpose: Run package/container compatibility extraction routes.
// Inputs: `archive_format` is concrete and `command` contains paths plus overwrite policy.
// Outputs: Returns operation statistics when the format belongs to this group; otherwise returns empty.
std::optional<superzip::OperationStats> extract_container_format(superzip::ArchiveFormat archive_format,
                                                                 const CliExtractCommand& command) {
    switch (archive_format) {
    case superzip::ArchiveFormat::Cab:
        reject_extract_tuning("CAB", command);
        return superzip::extract_cab(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Iso:
        reject_extract_tuning("ISO", command);
        return superzip::extract_iso(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Cpio:
        reject_extract_tuning("CPIO", command);
        return superzip::extract_cpio(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::CpioGzip:
        reject_extract_tuning("CPIO.GZ", command);
        return superzip::extract_cpio_gzip(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Ar:
    case superzip::ArchiveFormat::Deb:
        reject_extract_tuning("AR/DEB", command);
        return superzip::extract_ar(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Arj:
        reject_extract_tuning("ARJ", command);
        return superzip::extract_arj(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Arc:
        reject_extract_tuning("ARC", command);
        return superzip::extract_arc(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Rpm:
        reject_extract_tuning("RPM", command);
        return superzip::extract_rpm(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Lha:
        reject_extract_tuning("LHA", command);
        return superzip::extract_lha(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Wim:
        reject_extract_tuning("WIM", command);
        return superzip::extract_wim(command.archive, command.output, command.overwrite);
    case superzip::ArchiveFormat::Xar:
        reject_extract_tuning("XAR", command);
        return superzip::extract_xar(command.archive, command.output, command.overwrite);
    default:
        return std::nullopt;
    }
}

// Purpose: Run the selected extract backend after command-line validation.
// Inputs: `archive_format` is concrete and `command` contains parsed extraction options.
// Outputs: Returns operation statistics; throws for unsupported formats or backend errors.
superzip::OperationStats extract_by_format(superzip::ArchiveFormat archive_format, const CliExtractCommand& command) {
    if (archive_format == superzip::ArchiveFormat::SuperZip) {
        return extract_native_suzip(command);
    }
    if (auto result = extract_stream_or_tar_format(archive_format, command); result.has_value()) {
        return *result;
    }
    if (auto result = extract_container_format(archive_format, command); result.has_value()) {
        return *result;
    }
    throw superzip::ArchiveError(std::string("archive format recognized but not implemented for extraction: ") +
                                 superzip::archive_format_info(archive_format).key);
}

// Purpose: Execute the `extract` CLI command.
// Inputs: `args` is the full argument vector beginning with `extract`.
// Outputs: Returns a process exit code and writes operation telemetry to stdout.
int run_extract_command(const std::vector<std::string>& args) {
    const auto command = parse_extract_command(args);
    if (command.output.empty() || command.archive.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    if (command.sha256) {
        print_integrity_hash(command.archive);
    }
    if (command.defender_scan) {
        print_defender_scan(command.archive, true);
    }
    const auto archive_format = resolve_cli_archive_format(command.format, command.archive, true);
    reject_unsupported_cli_format(archive_format, "extract");
    print_stats(extract_by_format(archive_format, command));
    if (command.defender_scan) {
        print_defender_scan(command.output, false);
    }
    return 0;
}

// Purpose: Execute `memory-benchmark` after parsing bounded options.
// Inputs: `args` is the full argument vector beginning with `memory-benchmark`.
// Outputs: Returns zero and prints benchmark telemetry.
int run_memory_benchmark_command(const std::vector<std::string>& args) {
    superzip::cli::MemoryBenchmarkOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--size-mib") {
            options.size_mib = require_u32_arg(args, i, "--size-mib");
        } else if (args[i] == "--profile") {
            options.profile = require_arg(args, i, "--profile");
        } else if (args[i] == "--require-gpu") {
            options.require_gpu = true;
        } else if (args[i] == "--force-cpu") {
            options.force_cpu = true;
        } else if (args[i] == "--workers") {
            options.workers = require_u32_arg(args, i, "--workers");
        } else if (args[i] == "--block-size-kib") {
            options.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
        } else if (args[i] == "--compression-level") {
            options.compression_level = require_int_arg(args, i, "--compression-level", superzip::kMinCompressionLevel,
                                                        superzip::kMaxCompressionLevel);
        } else {
            throw superzip::ArchiveError("unknown memory-benchmark argument: " + args[i]);
        }
    }
    superzip::cli::print_memory_benchmark_stats(superzip::cli::run_memory_benchmark(options));
    return 0;
}

// Purpose: Execute `benchmark-suite` after parsing bounded options.
// Inputs: `args` is the full argument vector beginning with `benchmark-suite`.
// Outputs: Returns zero and prints benchmark-suite telemetry.
int run_benchmark_suite_command(const std::vector<std::string>& args) {
    superzip::cli::BenchmarkSuiteOptions options;
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
            options.compression_level = require_int_arg(args, i, "--compression-level", superzip::kMinCompressionLevel,
                                                        superzip::kMaxCompressionLevel);
        } else if (args[i] == "--tune") {
            options.tune = true;
        } else if (args[i] == "--tune-levels") {
            options.tune = true;
            options.tune_levels = true;
        } else {
            throw superzip::ArchiveError("unknown benchmark-suite argument: " + args[i]);
        }
    }
    superzip::cli::run_benchmark_suite(options);
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
        } else if (args[i] == "--inflight") {
            command.inflight = require_u32_arg(args, i, "--inflight");
        } else if (args[i] == "--sha256") {
            command.sha256 = true;
        } else if (args[i] == "--defender-scan") {
            command.defender_scan = true;
        } else {
            command.archive = args[i];
        }
    }
    return command;
}

// Purpose: Execute the `verify` CLI command for native SUZIP archives.
// Inputs: `args` is the full argument vector beginning with `verify`.
// Outputs: Returns a process exit code and writes verification telemetry to stdout.
int run_verify_command(const std::vector<std::string>& args) {
    const auto command = parse_verify_command(args);
    if (command.archive.empty()) {
        usage();
        return 2;
    }
    if (command.require_gpu && command.force_cpu) {
        throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    superzip::ExtractOptions options;
    options.gpu_required = command.require_gpu;
    options.force_cpu = command.force_cpu;
    options.worker_count = command.workers;
    options.max_inflight_chunks = command.inflight;
    print_stats(superzip::verify_suzip(command.archive, options));
    if (command.sha256) {
        print_integrity_hash(command.archive);
    }
    if (command.defender_scan) {
        print_defender_scan(command.archive, false);
    }
    return 0;
}

// Purpose: Execute non-archive informational CLI commands.
// Inputs: `args` is the full argument vector beginning with an informational command.
// Outputs: Returns the command exit code or `std::nullopt` when `args[0]` is not informational.
std::optional<int> run_info_command(const std::vector<std::string>& args) {
    if (args[0] == "gpu-info") {
        const auto info = superzip::query_gpu_info();
        print_gpu_info(info);
        return info.available ? 0 : 1;
    }
    if (args[0] == "dependency-check") {
        const auto info = superzip::query_gpu_info();
        print_gpu_info(info);
        const int code = dependency_exit_code(info);
        std::cout << "dependency_status="
                  << (code == 0    ? "ready"
                      : code == 10 ? "cpu_only_build"
                      : code == 11 ? "missing_hip_runtime"
                                   : "missing_amd_gpu")
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
        const auto format = resolve_cli_archive_format("auto", args[1], true);
        const auto& info = superzip::archive_format_info(format);
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
    superzip::GpuDiagnosticOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--seconds") {
            const auto seconds = require_int_arg(args, i, "--seconds", 1, 30);
            options.seconds = static_cast<double>(seconds);
        } else if (args[i] == "--buffer-mib") {
            options.buffer_mib = require_u32_arg(args, i, "--buffer-mib");
        } else if (args[i] == "--inner-iterations") {
            options.inner_iterations = require_u32_arg(args, i, "--inner-iterations");
        } else {
            throw superzip::ArchiveError("unknown gpu-diagnostic argument: " + args[i]);
        }
    }
    print_gpu_diagnostic(superzip::run_gpu_diagnostic(options));
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

// Purpose: Execute the SuperZip command-line interface.
// Inputs: `argc`/`argv` are process command-line arguments encoded by the platform C runtime.
// Outputs: Returns 0 on success, 1 on operation failure, and 2 on invalid usage.
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
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
