#include "test_util.hpp"

#define FSE_STATIC_LINKING_ONLY
extern "C" {
#include "common/fse.h"
}

#include <array>
#include <cstddef>

// Purpose: Verify packed FSE ABI and every supported descriptor geometry against independent word counts.
// Inputs: Real word allocations, table logs including RLE, and boundary symbol values.
// Outputs: Requires exact header values, alignment and nonoverlapping live state/transform regions.
TEST_CASE(zstd_fse_packed_regions_preserve_layout) {
    for (unsigned log = 0; log <= 15; ++log) {
        for (const unsigned maximum : {0U, 1U, 7U, 255U}) {
            const auto stateCount = log == 0 ? 2U : 1U << log;
            const auto stateWords = stateCount / 2U;
            const auto words = 1U + stateWords + 2U * (maximum + 1U);
            std::vector<FSE_CTable> storage(words, 0U);
            auto* bytes = reinterpret_cast<BYTE*>(storage.data());
            MEM_write16(bytes, static_cast<U16>(log));
            MEM_write16(bytes + 2U, static_cast<U16>(maximum));
            const auto header = FSE_readCTableHeader(storage.data());
            REQUIRE_EQ(header.tableLog, log);
            REQUIRE_EQ(header.maxSymbolValue, maximum);
            const auto geometry = FSE_getCTableGeometry(log, maximum);
            REQUIRE_EQ(geometry.stateCount, stateCount);
            REQUIRE_EQ(geometry.symbolCount, maximum + 1U);
            REQUIRE_EQ(geometry.transformOffset, (1U + stateWords) * sizeof(FSE_CTable));
            REQUIRE_EQ(geometry.requiredBytes, words * sizeof(FSE_CTable));
            REQUIRE_EQ(geometry.transformOffset % alignof(FSE_symbolCompressionTransform), 0U);
            FSE_CState_t state{};
            FSE_initCState(&state, storage.data());
            REQUIRE_TRUE(state.stateTable == bytes + 4U);
            REQUIRE_TRUE(state.symbolTT == bytes + geometry.transformOffset);
            REQUIRE_EQ(state.stateLog, log);
            REQUIRE_EQ(state.value, log == 0 ? 1U : stateCount);
        }
    }
}

// Purpose: Reject descriptors that could overflow shifts or exceed byte-symbol storage before deriving pointers.
// Inputs: Excessive table logs and maximum symbols, including unsigned maxima.
// Outputs: Requires no region/count publication for every invalid descriptor.
TEST_CASE(zstd_fse_packed_regions_reject_invalid_descriptors) {
    for (const auto values : std::array<std::array<unsigned, 2>, 4>{{{16U, 255U}, {~0U, 0U}, {12U, 256U}, {0U, ~0U}}}) {
        const auto geometry = FSE_getCTableGeometry(values[0], values[1]);
        REQUIRE_EQ(geometry.stateCount, 0U);
        REQUIRE_EQ(geometry.symbolCount, 0U);
        REQUIRE_EQ(geometry.transformOffset, 0U);
        REQUIRE_EQ(geometry.requiredBytes, 0U);
    }
}
