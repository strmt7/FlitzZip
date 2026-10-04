#include "test_util.hpp"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#include <array>
#include <memory>
#include <span>

namespace {
struct MatchControl {
    ZSTD_strategy strategy;
    std::size_t size;
    std::span<const unsigned char> frame;
};

// Captured before the progress rewrite from native inputs
// 27bf96a9183befb9efa4c1af3d2c7b04053b3b1e885826fa8e350dc216b415ac.
// These exact frames qualify behavior preservation, not compression superiority.
constexpr unsigned char lazyShort[]{0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x80, 0x45, 0x00, 0x00,
                                    0x00, 0x01, 0x00, 0x5D, 0xF4, 0x29, 0x04, 0x02};
constexpr unsigned char lazyMedium[]{0x28, 0xB5, 0x2F, 0xFD, 0x60, 0x00, 0x03, 0x45, 0x00,
                                     0x00, 0x00, 0x01, 0x00, 0xFD, 0x45, 0x5F, 0x41, 0x20};
constexpr unsigned char lazyLong[]{0x28, 0xB5, 0x2F, 0xFD, 0x60, 0x00, 0x1F, 0x85, 0x00, 0x00, 0x00, 0x03, 0x40,
                                   0x00, 0x3E, 0x00, 0x67, 0xEA, 0xBF, 0xE8, 0x1F, 0x9F, 0xF3, 0xA2, 0xF7, 0x61};
constexpr unsigned char optimalShort[]{0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x80, 0x4D, 0x00, 0x00,
                                       0x08, 0x6C, 0x01, 0x00, 0x5C, 0xF4, 0x29, 0x14, 0x02};
constexpr unsigned char optimalMedium[]{0x28, 0xB5, 0x2F, 0xFD, 0x60, 0x00, 0x03, 0x4D, 0x00, 0x00,
                                        0x08, 0x6C, 0x01, 0x00, 0xFC, 0x45, 0x5F, 0x41, 0x21};
constexpr unsigned char optimalLong[]{0x28, 0xB5, 0x2F, 0xFD, 0x60, 0x00, 0x1F, 0x9D, 0x00, 0x00,
                                      0x10, 0x6C, 0x00, 0x03, 0x00, 0x3E, 0x00, 0x67, 0x02, 0xFE,
                                      0x8B, 0xFE, 0xF1, 0x6F, 0xF3, 0xA2, 0xF7, 0x21, 0x1B};
constexpr std::array controls{
    MatchControl{ZSTD_btlazy2, 128, lazyShort},    MatchControl{ZSTD_btlazy2, 1024, lazyMedium},
    MatchControl{ZSTD_btlazy2, 8192, lazyLong},    MatchControl{ZSTD_btopt, 128, optimalShort},
    MatchControl{ZSTD_btopt, 1024, optimalMedium}, MatchControl{ZSTD_btopt, 8192, optimalLong},
    MatchControl{ZSTD_btultra, 128, optimalShort}, MatchControl{ZSTD_btultra, 1024, optimalMedium},
    MatchControl{ZSTD_btultra, 8192, optimalLong},
};

// Purpose: Compare a produced frame to independently captured pre-rewrite bytes.
// Inputs: Complete produced frame and the captured typed binary control.
// Outputs: Requires exact size and every byte, with no approximate digest comparison.
void require_frame(std::span<const unsigned char> frame, std::span<const unsigned char> expected) {
    REQUIRE_TRUE(std::ranges::equal(frame, expected));
}
}  // namespace

// Purpose: Preserve match decisions and budgets in attached dictionary search across real strategies.
// Inputs: Fixed raw dictionary and short/long exact-match corpora; dictionaries are forced to attach.
// Outputs: Requires byte-identical pre-rewrite frames and full independent dictionary decompression.
TEST_CASE(zstd_match_progress_preserves_attached_dictionary_frames) {
    std::array<unsigned char, 4096> dictionary{};
    for (std::size_t index = 0; index < dictionary.size(); ++index) {
        dictionary[index] = static_cast<unsigned char>((index * 37 + (index / 31) * 11) % 251);
    }
    for (const auto& control : controls) {
        std::vector<unsigned char> input(control.size);
        for (std::size_t index = 0; index < input.size(); ++index)
            input[index] = dictionary[(index + 97) % dictionary.size()];
        auto parameters = ZSTD_getCParams(13, input.size(), dictionary.size());
        parameters.strategy = control.strategy;
        const std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> prepared(
            ZSTD_createCDict_advanced(dictionary.data(), dictionary.size(), ZSTD_dlm_byCopy, ZSTD_dct_rawContent,
                                      parameters, {}),
            ZSTD_freeCDict);
        const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressor(ZSTD_createCCtx(), ZSTD_freeCCtx);
        const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressor(ZSTD_createDCtx(), ZSTD_freeDCtx);
        REQUIRE_TRUE(prepared && compressor && decompressor);
        REQUIRE_TRUE(!ZSTD_isError(ZSTD_CCtx_refCDict(compressor.get(), prepared.get())));
        REQUIRE_TRUE(
            !ZSTD_isError(ZSTD_CCtx_setParameter(compressor.get(), ZSTD_c_forceAttachDict, ZSTD_dictForceAttach)));
        std::vector<unsigned char> frame(ZSTD_compressBound(input.size()));
        const auto written = ZSTD_compress2(compressor.get(), frame.data(), frame.size(), input.data(), input.size());
        REQUIRE_TRUE(!ZSTD_isError(written));
        require_frame(std::span(frame).first(written), control.frame);
        std::vector<unsigned char> restored(input.size());
        REQUIRE_EQ(ZSTD_decompress_usingDict(decompressor.get(), restored.data(), restored.size(), frame.data(),
                                             written, dictionary.data(), dictionary.size()),
                   input.size());
        REQUIRE_TRUE(restored == input);
    }
}
