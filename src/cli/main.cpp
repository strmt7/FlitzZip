#include "core/archive.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "zip/zip_adapter.hpp"

#include <filesystem>
#include <iostream>
#include <string>
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
        << "  superzip_cli compress --format szip|zip --output <archive> [--require-gpu] [--sha256] [--defender-scan] <path>...\n"
        << "  superzip_cli extract --format szip|zip --output <directory> [--require-gpu] [--overwrite] [--sha256] [--defender-scan] <archive>\n"
        << "  superzip_cli verify [--sha256] [--defender-scan] <archive.szip>\n";
}

// Purpose: Convert byte/second statistics to MiB/s for display.
// Inputs: `bytes` is the processed byte count and `seconds` is elapsed wall time.
// Outputs: Returns MiB per second, or zero when elapsed time is zero.
double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
}

// Purpose: Print archive operation statistics in a stable key/value format.
// Inputs: `stats` is the completed operation result.
// Outputs: Writes one line to stdout.
void print_stats(const superzip::OperationStats& stats) {
    std::cout << "entries=" << stats.entries
              << " input_bytes=" << stats.input_bytes
              << " output_bytes=" << stats.output_bytes
              << " gpu_used=" << (stats.gpu_used ? "true" : "false")
              << " seconds=" << stats.seconds
              << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
              << "\n";
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
// Inputs: `path` is the target file or directory and `block_if_detected` controls whether a non-clean attempted scan aborts the operation.
// Outputs: Writes scan state to stdout; throws `SecurityError` only when Defender reports a scanned target is not clean and blocking is requested.
void print_defender_scan(const std::filesystem::path& path, bool block_if_detected) {
    const auto scan = superzip::scan_with_windows_defender(path, superzip::DefenderScanMode::FullPath);
    std::cout << "defender_attempted=" << (scan.attempted ? "true" : "false") << "\n";
    std::cout << "defender_clean=" << (scan.clean ? "true" : "false") << "\n";
    std::cout << "defender_exit_code=" << scan.exit_code << "\n";
    if (block_if_detected && scan.attempted && !scan.clean) {
        throw superzip::SecurityError("Microsoft Defender did not report the target as clean: " + path.string());
    }
}

// Purpose: Read the value following a named command-line flag.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag label for diagnostics.
// Outputs: Returns the following argument value; throws `ArchiveError` when the value is missing.
std::string require_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    if (index + 1 >= args.size()) {
        throw superzip::ArchiveError(std::string("missing value for ") + name);
    }
    ++index;
    return args[index];
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

        if (args[0] == "gpu-info") {
            const auto info = superzip::query_gpu_info();
            std::cout << "hip_compiled=" << (info.hip_compiled ? "true" : "false") << "\n";
            std::cout << "available=" << (info.available ? "true" : "false") << "\n";
            std::cout << "device_count=" << info.device_count << "\n";
            std::cout << "selected_device=" << info.selected_device << "\n";
            std::cout << "device_name=" << info.device_name << "\n";
            std::cout << "gcn_arch=" << info.gcn_arch << "\n";
            std::cout << "status=" << info.status << "\n";
            return info.available ? 0 : 1;
        }

        if (args[0] == "compress") {
            std::string format = "szip";
            bool require_gpu = false;
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path output;
            std::vector<std::filesystem::path> sources;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--format") {
                    format = require_arg(args, i, "--format");
                } else if (args[i] == "--output") {
                    output = require_arg(args, i, "--output");
                } else if (args[i] == "--require-gpu") {
                    require_gpu = true;
                } else if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    sources.emplace_back(args[i]);
                }
            }
            if (output.empty() || sources.empty()) {
                usage();
                return 2;
            }
            if (format == "szip") {
                superzip::CompressOptions options;
                options.gpu_required = require_gpu;
                print_stats(superzip::compress_szip(sources, output, options));
            } else if (format == "zip") {
                print_stats(superzip::compress_zip(sources, output));
            } else {
                throw superzip::ArchiveError("unknown archive format: " + format);
            }
            if (sha256) {
                print_integrity_hash(output);
            }
            if (defender_scan) {
                print_defender_scan(output, false);
            }
            return 0;
        }

        if (args[0] == "extract") {
            std::string format = "szip";
            bool require_gpu = false;
            bool overwrite = false;
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path output;
            std::filesystem::path archive;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--format") {
                    format = require_arg(args, i, "--format");
                } else if (args[i] == "--output") {
                    output = require_arg(args, i, "--output");
                } else if (args[i] == "--require-gpu") {
                    require_gpu = true;
                } else if (args[i] == "--overwrite") {
                    overwrite = true;
                } else if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    archive = args[i];
                }
            }
            if (output.empty() || archive.empty()) {
                usage();
                return 2;
            }
            if (sha256) {
                print_integrity_hash(archive);
            }
            if (defender_scan) {
                print_defender_scan(archive, true);
            }
            if (format == "szip") {
                superzip::ExtractOptions options;
                options.gpu_required = require_gpu;
                options.overwrite = overwrite;
                print_stats(superzip::extract_szip(archive, output, options));
            } else if (format == "zip") {
                print_stats(superzip::extract_zip(archive, output, overwrite));
            } else {
                throw superzip::ArchiveError("unknown archive format: " + format);
            }
            if (defender_scan) {
                print_defender_scan(output, false);
            }
            return 0;
        }

        if (args[0] == "verify") {
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path archive;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    archive = args[i];
                }
            }
            if (archive.empty()) {
                usage();
                return 2;
            }
            superzip::ExtractOptions options;
            options.gpu_required = false;
            print_stats(superzip::verify_szip(archive, options));
            if (sha256) {
                print_integrity_hash(archive);
            }
            if (defender_scan) {
                print_defender_scan(archive, false);
            }
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
