#pragma once

#include "CpuArch.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace superzip_test {

// Purpose: Require each SDK macro to evaluate pointer/value expressions once.
// Inputs: UInt fixes the wire width; callbacks expand the real production macros.
// Outputs: Rejects repeated expressions or a changed value after the store/load.
template <typename UInt, typename Load, typename Store> void check_sdk_argument_evaluation(Load load, Store store) {
    std::array<Byte, sizeof(UInt)> bytes{};
    unsigned pointer_calls = 0;
    unsigned value_calls = 0;
    auto pointer = [&] {
        ++pointer_calls;
        return bytes.data();
    };
    constexpr auto expected = static_cast<UInt>(0xFEDCBA9876543210ULL);
    auto value = [&] {
        ++value_calls;
        return expected;
    };
    store(pointer, value);
    if (pointer_calls != 1 || value_calls != 1) {
        throw std::runtime_error("SDK store repeats a pointer or value expression");
    }
    const auto actual = load(pointer);
    if (pointer_calls != 2 || value_calls != 1 || actual != expected) {
        throw std::runtime_error("SDK load repeats a pointer expression or changes wire bits");
    }
}

// Purpose: Check SDK integer access against independent byte encoding at every alignment offset.
// Inputs: UInt fixes field width; load/store are SDK operations; big_endian selects wire order.
// Outputs: Throws on wrong bytes, values, or writes outside the field, including at the buffer end.
template <typename UInt, typename Load, typename Store> void check_sdk_access(Load load, Store store, bool big_endian) {
    constexpr auto width = sizeof(UInt);
    constexpr std::array<std::uint64_t, 6> patterns{
        0, 1, 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0x8080808080808080ULL, UINT64_MAX};
    for (std::size_t offset = 0; offset < 16; ++offset) {
        for (const auto pattern : patterns) {
            std::array<Byte, 24> bytes{};
            bytes.fill(0xA5);
            const auto value = static_cast<UInt>(pattern);
            store(bytes.data() + offset, value);
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                auto expected = static_cast<Byte>(0xA5);
                if (index >= offset && index - offset < width) {
                    const auto byte = index - offset;
                    const auto shift = 8 * (big_endian ? width - 1 - byte : byte);
                    expected = static_cast<Byte>(static_cast<std::uint64_t>(value) >> shift);
                }
                if (bytes[index] != expected) {
                    throw std::runtime_error("SDK integer store disagrees with byte oracle or changes a canary");
                }
            }
            if (static_cast<std::uint64_t>(load(bytes.data() + offset)) != static_cast<std::uint64_t>(value)) {
                throw std::runtime_error("SDK integer load disagrees with byte oracle");
            }
        }
    }
    std::array<Byte, width> boundary{};
    store(boundary.data(), std::numeric_limits<UInt>::max());
    if (static_cast<std::uint64_t>(load(boundary.data())) !=
        static_cast<std::uint64_t>(std::numeric_limits<UInt>::max())) {
        throw std::runtime_error("SDK integer exact-width boundary access failed");
    }
}

// Purpose: Preserve all unaligned SDK little/big-endian operations without typed pointer dereferences.
// Inputs: Six widths/orders, six bit patterns, all offsets 0-15, and exact-width buffers.
// Outputs: Byte-oracle, canary, and round-trip assertions for every case.
inline void verify_sdk_unaligned_access() {
    check_sdk_access<UInt16>([](const Byte* p) { return GetUi16(p); }, [](Byte* p, UInt16 v) { SetUi16(p, v); }, false);
    check_sdk_access<UInt32>([](const Byte* p) { return GetUi32(p); }, [](Byte* p, UInt32 v) { SetUi32(p, v); }, false);
    check_sdk_access<UInt64>([](const Byte* p) { return GetUi64(p); }, [](Byte* p, UInt64 v) { SetUi64(p, v); }, false);
    check_sdk_access<UInt16>([](const Byte* p) { return GetBe16(p); }, [](Byte* p, UInt16 v) { SetBe16(p, v); }, true);
    check_sdk_access<UInt32>([](const Byte* p) { return GetBe32(p); }, [](Byte* p, UInt32 v) { SetBe32(p, v); }, true);
    check_sdk_access<UInt64>([](const Byte* p) { return GetBe64(p); }, [](Byte* p, UInt64 v) { SetBe64(p, v); }, true);
    check_sdk_argument_evaluation<UInt16>([](auto& p) { return GetUi16(p()); },
                                          [](auto& p, auto& v) { SetUi16(p(), v()); });
    check_sdk_argument_evaluation<UInt32>([](auto& p) { return GetUi32(p()); },
                                          [](auto& p, auto& v) { SetUi32(p(), v()); });
    check_sdk_argument_evaluation<UInt64>([](auto& p) { return GetUi64(p()); },
                                          [](auto& p, auto& v) { SetUi64(p(), v()); });
    check_sdk_argument_evaluation<UInt16>([](auto& p) { return GetBe16(p()); },
                                          [](auto& p, auto& v) { SetBe16(p(), v()); });
    check_sdk_argument_evaluation<UInt32>([](auto& p) { return GetBe32(p()); },
                                          [](auto& p, auto& v) { SetBe32(p(), v()); });
    check_sdk_argument_evaluation<UInt64>([](auto& p) { return GetBe64(p()); },
                                          [](auto& p, auto& v) { SetBe64(p(), v()); });
    check_sdk_argument_evaluation<UInt16>([](auto& p) { return GetBe16_to32(p()); },
                                          [](auto& p, auto& v) { SetBe16(p(), v()); });
}

// Purpose: Verify aligned-name aliases preserve wire semantics on byte buffers without extra assumptions.
// Inputs: The SDK's aligned-name load aliases and 16/32-bit stores at every tested byte offset.
// Outputs: Byte-oracle/canary assertions; aliases agree with their unaligned wire formats.
inline void verify_sdk_aligned_access() {
    check_sdk_access<UInt16>([](const Byte* p) { return GetUi16a(p); }, [](Byte* p, UInt16 v) { SetUi16a(p, v); },
                             false);
    check_sdk_access<UInt32>([](const Byte* p) { return GetUi32a(p); }, [](Byte* p, UInt32 v) { SetUi32a(p, v); },
                             false);
    check_sdk_access<UInt64>([](const Byte* p) { return GetUi64a(p); }, [](Byte* p, UInt64 v) { SetUi64(p, v); },
                             false);
    check_sdk_access<UInt16>([](const Byte* p) { return GetBe16a(p); }, [](Byte* p, UInt16 v) { SetBe16a(p, v); },
                             true);
    check_sdk_access<UInt32>([](const Byte* p) { return GetBe32a(p); }, [](Byte* p, UInt32 v) { SetBe32a(p, v); },
                             true);
    check_sdk_access<UInt64>([](const Byte* p) { return GetBe64a(p); }, [](Byte* p, UInt64 v) { SetBe64(p, v); }, true);
    check_sdk_argument_evaluation<UInt16>([](auto& p) { return GetUi16a(p()); },
                                          [](auto& p, auto& v) { SetUi16a(p(), v()); });
    check_sdk_argument_evaluation<UInt32>([](auto& p) { return GetUi32a(p()); },
                                          [](auto& p, auto& v) { SetUi32a(p(), v()); });
    check_sdk_argument_evaluation<UInt64>([](auto& p) { return GetUi64a(p()); },
                                          [](auto& p, auto& v) { SetUi64(p(), v()); });
    check_sdk_argument_evaluation<UInt16>([](auto& p) { return GetBe16a(p()); },
                                          [](auto& p, auto& v) { SetBe16a(p(), v()); });
    check_sdk_argument_evaluation<UInt32>([](auto& p) { return GetBe32a(p()); },
                                          [](auto& p, auto& v) { SetBe32a(p(), v()); });
    check_sdk_argument_evaluation<UInt64>([](auto& p) { return GetBe64a(p()); },
                                          [](auto& p, auto& v) { SetBe64(p(), v()); });
}

}  // namespace superzip_test
