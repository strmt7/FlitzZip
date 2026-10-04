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

// Purpose: Prove expression checks reject repeated pointers and values.
// Inputs: Controlled callbacks repeat an expression while using actual wire accessors.
// Outputs: Both negative controls must throw; positive controls run in the shared oracle.
TEST_CASE(sdk_byte_access_argument_oracle_rejects_repetition) {
    bool repeated_pointer_rejected = false;
    try {
        superzip_test::check_sdk_argument_evaluation<UInt16>(
            // Purpose: Repeat a pointer callback to model a broken macro.
            // Inputs: Counting pointer callback. Outputs: Correct bits with an invalid call count.
            [](auto& pointer) {
                const auto* data = pointer();
                (void)pointer();
                return GetUi16(data);
            },
            [](auto& pointer, auto& value) { SetUi16(pointer(), value()); });
    } catch (const std::runtime_error&) {
        repeated_pointer_rejected = true;
    }
    if (!repeated_pointer_rejected) {
        throw std::runtime_error("SDK repeated pointer negative control was accepted");
    }
    bool repeated_value_rejected = false;
    try {
        // Purpose: Repeat a value callback to model a broken store macro.
        // Inputs: Counting callbacks. Outputs: Correct bytes with an invalid call count.
        const auto repeated_value = [](auto& pointer, auto& value) {
            const auto bits = value();
            (void)value();
            SetUi16(pointer(), bits);
        };
        superzip_test::check_sdk_argument_evaluation<UInt16>([](auto& pointer) { return GetUi16(pointer()); },
                                                             repeated_value);
    } catch (const std::runtime_error&) {
        repeated_value_rejected = true;
    }
    if (!repeated_value_rejected) {
        throw std::runtime_error("SDK repeated value negative control was accepted");
    }
}
