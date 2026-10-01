#include "sdk_byte_access_checks.hpp"
#include "test_util.hpp"

// Purpose: Run the shared independent byte oracle for unaligned SDK integer access.
// Inputs: All width/order/pattern/offset cases in the portable test helper.
// Outputs: Throws on wrong bytes, canary damage, or boundary access failure.
TEST_CASE(sdk_byte_access_unaligned_endian_oracle) {
    superzip_test::verify_sdk_unaligned_access();
}

// Purpose: Run the same oracle for SDK aligned-name aliases on byte buffers.
// Inputs: Portable alias checks also exercised by the Linux sanitizer fuzzer initialization.
// Outputs: Throws on wire-semantic or bounded-access failures.
TEST_CASE(sdk_byte_access_aligned_alias_oracle) {
    superzip_test::verify_sdk_aligned_access();
}
