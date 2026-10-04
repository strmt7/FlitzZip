#define SUPERZIP_SDK_DEFINE_WIRE_ABI
#include "CpuArchByteAccess.h"

// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 2 readable bytes. Outputs: The field value, with no retained pointer.
UInt16 Z7_ReadLE16(const void* p) noexcept {
    return superzip_sdk::read_field<UInt16, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 2 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteLE16(void* p, UInt16 value) noexcept {
    superzip_sdk::write_field<UInt16, std::endian::little>(p, value);
}
// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 4 readable bytes. Outputs: The field value, with no retained pointer.
UInt32 Z7_ReadLE32(const void* p) noexcept {
    return superzip_sdk::read_field<UInt32, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 4 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteLE32(void* p, UInt32 value) noexcept {
    superzip_sdk::write_field<UInt32, std::endian::little>(p, value);
}
// Purpose: Decode one little-endian wire field through typed owned storage.
// Inputs: p owns 8 readable bytes. Outputs: The field value, with no retained pointer.
UInt64 Z7_ReadLE64(const void* p) noexcept {
    return superzip_sdk::read_field<UInt64, std::endian::little>(p);
}
// Purpose: Encode one little-endian wire field through typed owned storage.
// Inputs: p owns 8 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteLE64(void* p, UInt64 value) noexcept {
    superzip_sdk::write_field<UInt64, std::endian::little>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 2 readable bytes. Outputs: The field value, with no retained pointer.
UInt16 Z7_ReadBE16(const void* p) noexcept {
    return superzip_sdk::read_field<UInt16, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 2 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteBE16(void* p, UInt16 value) noexcept {
    superzip_sdk::write_field<UInt16, std::endian::big>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 4 readable bytes. Outputs: The field value, with no retained pointer.
UInt32 Z7_ReadBE32(const void* p) noexcept {
    return superzip_sdk::read_field<UInt32, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 4 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteBE32(void* p, UInt32 value) noexcept {
    superzip_sdk::write_field<UInt32, std::endian::big>(p, value);
}
// Purpose: Decode one big-endian wire field through typed owned storage.
// Inputs: p owns 8 readable bytes. Outputs: The field value, with no retained pointer.
UInt64 Z7_ReadBE64(const void* p) noexcept {
    return superzip_sdk::read_field<UInt64, std::endian::big>(p);
}
// Purpose: Encode one big-endian wire field through typed owned storage.
// Inputs: p owns 8 writable bytes; value contains bits. Outputs: Exactly the field bytes.
void Z7_WriteBE64(void* p, UInt64 value) noexcept {
    superzip_sdk::write_field<UInt64, std::endian::big>(p, value);
}
