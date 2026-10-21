#include "test_util.hpp"

#include "ar/ar_adapter.hpp"
#include "arc/arc_adapter.hpp"
#include "arj/arj_adapter.hpp"
#include "core/archive.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"
#include "iso/iso_adapter.hpp"
#include "rpm/rpm_format.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct MissingArchiveCase {
    std::string_view format;
    std::string_view extension;
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)> invoke;
};

// Purpose: Keep missing-file failures inside FlitzZip's public error hierarchy across native parser adapters.
// Inputs: Nonexistent archive paths for every adapter that requires a checked size before opening metadata.
// Outputs: Requires the shared UTF-8 path diagnostic and proves no destination is created by failed admission.
TEST_CASE(adapter_missing_archives_use_shared_product_error_contract) {
    const std::vector<MissingArchiveCase> cases{
        {"AR", ".ar",
         [](const auto& source, const auto& destination) { (void)flitzzip::extract_ar(source, destination, false); }},
        {"ARC", ".arc",
         [](const auto& source, const auto& destination) { (void)flitzzip::extract_arc(source, destination, false); }},
        {"ARJ", ".arj",
         [](const auto& source, const auto& destination) { (void)flitzzip::extract_arj(source, destination, false); }},
        {"ISO", ".iso",
         [](const auto& source, const auto& destination) { (void)flitzzip::extract_iso(source, destination, false); }},
        {"RPM", ".rpm", [](const auto& source, const auto&) { (void)flitzzip::scan_rpm_payload(source); }},
        {"FLZIP extract", ".flzip",
         [](const auto& source, const auto& destination) {
             flitzzip::ExtractOptions options;
             options.force_cpu = true;
             (void)flitzzip::extract_flzip(source, destination, options);
         }},
        {"FLZIP verify", ".flzip",
         [](const auto& source, const auto&) {
             flitzzip::ExtractOptions options;
             options.force_cpu = true;
             (void)flitzzip::verify_flzip(source, options);
         }},
    };

    const auto root = test_temp_dir("adapter-missing-contract");
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& candidate = cases[index];
        const auto archive =
            root / (std::string("missing-") + std::to_string(index) + std::string(candidate.extension));
        const auto destination = root / (std::string("out-") + std::to_string(index));
        bool rejected = false;
        try {
            candidate.invoke(archive, destination);
        } catch (const flitzzip::ArchiveError& error) {
            const auto expected_path = flitzzip::path_diagnostic_utf8(archive);
            if (std::string_view(error.what()).find(expected_path) == std::string_view::npos) {
                throw std::runtime_error(std::string(candidate.format) +
                                         " missing-source diagnostic mismatch: " + error.what());
            }
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(!std::filesystem::exists(destination));
    }
    std::filesystem::remove_all(root);
}

}  // namespace
