/* SuperZip typed wire integer access; see README.SUPERZIP.md for provenance. */
#ifndef SUPERZIP_SDK_BYTE_ACCESS_H
#define SUPERZIP_SDK_BYTE_ACCESS_H
#include "7zTypes.h"

#ifdef __cplusplus
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace superzip_sdk {
// Purpose: Convert an integer value between opposite byte orders without accessing memory.
// Inputs: An unsigned 16-, 32- or 64-bit value. Outputs: The same bits in reverse byte order.
template <typename UInt> inline UInt reverse_value(UInt value) noexcept {
    static_assert(sizeof(UInt) == 2 || sizeof(UInt) == 4 || sizeof(UInt) == 8);
#if defined(_MSC_VER)
    if constexpr (sizeof(UInt) == 2) {
        return _byteswap_ushort(value);
    } else if constexpr (sizeof(UInt) == 4) {
        return _byteswap_ulong(value);
    } else {
        return _byteswap_uint64(value);
    }
#elif defined(__GNUC__) || defined(__clang__)
    if constexpr (sizeof(UInt) == 2) {
        return __builtin_bswap16(value);
    } else if constexpr (sizeof(UInt) == 4) {
        return __builtin_bswap32(value);
    } else {
        return __builtin_bswap64(value);
    }
#else
    UInt reversed = 0;
    for (size_t byte = 0; byte < sizeof(UInt); ++byte) {
        reversed = static_cast<UInt>((reversed << 8) | (value & 0xff));
        value >>= 8;
    }
    return reversed;
#endif
}
// Purpose: Decode a completely initialized fixed-width wire field.
// Inputs: p owns sizeof(UInt) readable bytes; WireOrder is the field's byte order.
// Outputs: A value; local owned bytes and fixed spans never escape the call.
template <typename UInt, std::endian WireOrder> inline UInt read_field(const void* p) noexcept {
    static_assert(std::numeric_limits<UInt>::digits == sizeof(UInt) * 8);
    static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
    std::array<Byte, sizeof(UInt)> owned{};
    const std::span<const Byte, sizeof(UInt)> source(static_cast<const Byte*>(p), sizeof(UInt));
    std::copy(source.begin(), source.end(), owned.begin());
    const UInt value = std::bit_cast<UInt>(owned);
    if constexpr (std::endian::native != WireOrder) {
        return reverse_value(value);
    }
    return value;
}
// Purpose: Encode a value into one fixed-width wire field.
// Inputs: p owns sizeof(UInt) writable bytes; value has no padding bits.
// Outputs: Exactly the field's bytes; ordered owned storage bounds the transfer.
template <typename UInt, std::endian WireOrder> inline void write_field(void* p, UInt value) noexcept {
    static_assert(std::numeric_limits<UInt>::digits == sizeof(UInt) * 8);
    static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
    if constexpr (std::endian::native != WireOrder) {
        value = reverse_value(value);
    }
    const auto owned = std::bit_cast<std::array<Byte, sizeof(UInt)>>(value);
    const std::span<Byte, sizeof(UInt)> output(static_cast<Byte*>(p), sizeof(UInt));
    std::copy(owned.begin(), owned.end(), output.begin());
}
}  // namespace superzip_sdk
#endif

#if defined(__cplusplus) && !defined(SUPERZIP_SDK_DEFINE_WIRE_ABI)
// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 2 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt16 Z7_ReadLE16(const void* p) noexcept {
    return superzip_sdk::read_field<UInt16, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 2 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteLE16(void* p, UInt16 value) noexcept {
    superzip_sdk::write_field<UInt16, std::endian::little>(p, value);
}
// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 4 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt32 Z7_ReadLE32(const void* p) noexcept {
    return superzip_sdk::read_field<UInt32, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 4 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteLE32(void* p, UInt32 value) noexcept {
    superzip_sdk::write_field<UInt32, std::endian::little>(p, value);
}
// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 8 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt64 Z7_ReadLE64(const void* p) noexcept {
    return superzip_sdk::read_field<UInt64, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 8 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteLE64(void* p, UInt64 value) noexcept {
    superzip_sdk::write_field<UInt64, std::endian::little>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 2 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt16 Z7_ReadBE16(const void* p) noexcept {
    return superzip_sdk::read_field<UInt16, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 2 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteBE16(void* p, UInt16 value) noexcept {
    superzip_sdk::write_field<UInt16, std::endian::big>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 4 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt32 Z7_ReadBE32(const void* p) noexcept {
    return superzip_sdk::read_field<UInt32, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 4 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteBE32(void* p, UInt32 value) noexcept {
    superzip_sdk::write_field<UInt32, std::endian::big>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 8 readable bytes. Outputs: The field value, with no retained pointer.
static inline UInt64 Z7_ReadBE64(const void* p) noexcept {
    return superzip_sdk::read_field<UInt64, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 8 writable bytes; value contains bits. Outputs: Exactly the field bytes.
static inline void Z7_WriteBE64(void* p, UInt64 value) noexcept {
    superzip_sdk::write_field<UInt64, std::endian::big>(p, value);
}
#else
#ifdef __cplusplus
#define Z7_WIRE_NOTHROW noexcept
#else
#define Z7_WIRE_NOTHROW
#endif
/* C callers retain the same validated fixed-field preconditions. */
EXTERN_C_BEGIN
UInt16 Z7_ReadLE16(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteLE16(void* p, UInt16 value) Z7_WIRE_NOTHROW;
UInt32 Z7_ReadLE32(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteLE32(void* p, UInt32 value) Z7_WIRE_NOTHROW;
UInt64 Z7_ReadLE64(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteLE64(void* p, UInt64 value) Z7_WIRE_NOTHROW;
UInt16 Z7_ReadBE16(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteBE16(void* p, UInt16 value) Z7_WIRE_NOTHROW;
UInt32 Z7_ReadBE32(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteBE32(void* p, UInt32 value) Z7_WIRE_NOTHROW;
UInt64 Z7_ReadBE64(const void* p) Z7_WIRE_NOTHROW;
void Z7_WriteBE64(void* p, UInt64 value) Z7_WIRE_NOTHROW;
EXTERN_C_END
#undef Z7_WIRE_NOTHROW
#endif
#endif
