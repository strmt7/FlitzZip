#pragma once

#include "gpu/dictionary_matcher.hpp"
#include "gpu/hip_codec_support.hpp"

#include <cstdint>
#include <limits>

namespace superzip::dictionary::optimal {

inline constexpr std::uint32_t kLanes = 256U;
inline constexpr std::uint32_t kTreeWords = 2U * kSegmentBytes;
inline constexpr std::uint32_t kResidueTreeWords = 512U;
inline constexpr std::uint32_t kPositionsPerLaunch = kNeutronParseTilePositions;
inline constexpr std::uint32_t kSearchPositionsPerLaunch = kNeutronSearchTileBytes;
inline constexpr std::uint32_t kSequencesPerLaunch = kNeutronEmitSequences;
inline constexpr auto kInfinity = std::numeric_limits<std::uint64_t>::max();

struct State {
    Match* matches;
    std::uint64_t* suffix_tree;
    std::uint64_t* residue_tree;
    std::uint32_t* match_costs;
    std::uint32_t* match_ends;
    std::uint32_t* next_matches;
};

struct EmitCursor {
    std::uint32_t position;
    std::uint32_t written;
};

}  // namespace superzip::dictionary::optimal
