#pragma once

#include <ostream>
#include <string_view>

namespace flitzzip::cli {

inline constexpr std::string_view kBlockSizeUsage = "256|512|1024|2048|4096|8192|16384";

// Purpose: Keep CLI help presentation separate from command parsing and execution.
// Inputs: Borrowed output stream. Outputs: Existing complete help text; caller stream policy remains in force.
inline void print_usage(std::ostream& output) {
    output
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
           "[--defender-scan] <archive.flzip|archive.zip|archive.zipx> (GPU options: FLZIP only)\n"
        << "  Use -- before positional paths in compress, extract, or verify to preserve option-shaped filenames.\n";
}

}  // namespace flitzzip::cli
