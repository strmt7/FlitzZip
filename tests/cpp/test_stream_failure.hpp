#pragma once

#include "test_util.hpp"
#include "core/result.hpp"

#include <array>
#include <filesystem>

// Purpose: Prove a decoder cannot recover from a malformed read after stream flags are cleared.
// Inputs: Stream is a compatibility input stream; archive is a bounded malformed fixture.
// Outputs: Requires repeated finalization and reads to reject the original decoder failure.
template <typename Stream> void require_sticky_decoder_failure(const std::filesystem::path& archive) {
    Stream input(archive);
    std::array<char, 4096> bytes{};
    try {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    } catch (const superzip::Error&) {
    }
    REQUIRE_TRUE(input.bad());
    for (int attempt = 0; attempt < 2; ++attempt) {
        input.clear();
        bool finish_rejected = false;
        try {
            input.finish();
        } catch (const superzip::Error&) {
            finish_rejected = true;
        }
        REQUIRE_TRUE(finish_rejected);
        input.clear();
        input.exceptions(std::ios::goodbit);
        try {
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        } catch (const superzip::Error&) {
        }
        REQUIRE_TRUE(input.bad());
    }
    Stream finish_only(archive);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try {
            finish_only.finish();
        } catch (const superzip::Error&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}
