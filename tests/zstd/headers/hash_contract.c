/* Purpose: Exercise all supported classic hash operations across bounded input
 * lengths, seeds, alignments and payloads. Inputs: Compile-time API mode selects
 * external, private, inline or staged inclusion; one mode omits 64-bit APIs.
 * Outputs: Returns zero only when streaming, copied state and canonical
 * conversions agree with one-shot hashing in the original or owned dependency. */
#if SUPERZIP_HASH_MODE == 1
#define XXH_PRIVATE_API
#elif SUPERZIP_HASH_MODE == 2
#define XXH_INLINE_ALL
#elif SUPERZIP_HASH_MODE == 3
#include "common/xxhash.h"
#define XXH_STATIC_LINKING_ONLY
#include "common/xxhash.h"
#define XXH_INLINE_ALL
#else
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#endif
#if SUPERZIP_HASH_MODE == 4
#define XXH_NO_LONG_LONG
#endif
#include "common/xxhash.h"
#include "common/xxhash.h"

#ifndef XXH_NO_XXH3
#error Zstandard must keep XXH3 outside its supported hash contract.
#endif

/* Purpose: Compare stack-owned streaming state with one-shot/canonical results.
 * Inputs: A valid byte span, bounded length and seed for XXH32.
 * Outputs: Returns one only if all classic 32-bit API operations agree. */
static int check32(const unsigned char* input, size_t length, XXH32_hash_t seed) {
    XXH32_state_t state;
    XXH32_state_t copied;
    XXH32_canonical_t canonical;
    size_t cursor = 0;
    const XXH32_hash_t expected = XXH32(input, length, seed);
    if (XXH32_reset(&state, seed) != XXH_OK) {
        return 0;
    }
    while (cursor < length) {
        size_t count = cursor % 31 + 1;
        if (count > length - cursor) {
            count = length - cursor;
        }
        if (XXH32_update(&state, input + cursor, count) != XXH_OK) {
            return 0;
        }
        cursor += count;
    }
    XXH32_copyState(&copied, &state);
    XXH32_canonicalFromHash(&canonical, expected);
    return XXH32_digest(&state) == expected && XXH32_digest(&copied) == expected &&
           XXH32_hashFromCanonical(&canonical) == expected;
}

#ifndef XXH_NO_LONG_LONG
/* Purpose: Compare stack-owned streaming state with one-shot/canonical results.
 * Inputs: A valid byte span, bounded length and seed for XXH64.
 * Outputs: Returns one only if all classic 64-bit API operations agree. */
static int check64(const unsigned char* input, size_t length, XXH64_hash_t seed) {
    XXH64_state_t state;
    XXH64_state_t copied;
    XXH64_canonical_t canonical;
    size_t cursor = 0;
    const XXH64_hash_t expected = XXH64(input, length, seed);
    if (XXH64_reset(&state, seed) != XXH_OK) {
        return 0;
    }
    while (cursor < length) {
        size_t count = cursor % 31 + 1;
        if (count > length - cursor) {
            count = length - cursor;
        }
        if (XXH64_update(&state, input + cursor, count) != XXH_OK) {
            return 0;
        }
        cursor += count;
    }
    XXH64_copyState(&copied, &state);
    XXH64_canonicalFromHash(&canonical, expected);
    return XXH64_digest(&state) == expected && XXH64_digest(&copied) == expected &&
           XXH64_hashFromCanonical(&canonical) == expected;
}
#endif

/* Purpose: Cover empty through 4 KiB inputs at every alignment and two seeds.
 * Inputs: Three deterministic profiles in a fixed stack buffer; no allocation.
 * Outputs: Returns zero for 98,328 agreeing cases, or one at the first mismatch. */
int main(void) {
    unsigned char buffer[4100];
    unsigned profile;
    for (profile = 0; profile < 3; ++profile) {
        unsigned random = 12345;
        size_t index;
        size_t length;
        for (index = 0; index < sizeof(buffer); ++index) {
            random = random * 1664525u + 1013904223u;
            buffer[index] = profile == 0 ? (unsigned char)(random >> 24) : (profile == 1 ? (unsigned char)index : 0);
        }
        for (length = 0; length <= sizeof(buffer) - 4; ++length) {
            unsigned alignment;
            for (alignment = 0; alignment < 4; ++alignment) {
                unsigned seeded;
                for (seeded = 0; seeded < 2; ++seeded) {
                    const XXH32_hash_t seed32 = seeded ? 0x9e3779b1u : 0;
                    if (!check32(buffer + alignment, length, seed32)) {
                        return 1;
                    }
#ifndef XXH_NO_LONG_LONG
                    {
                        const XXH64_hash_t seed64 = seeded ? 0x9e3779b185ebca87ULL : 0;
                        if (!check64(buffer + alignment, length, seed64)) {
                            return 1;
                        }
                    }
#endif
                }
            }
        }
    }
    return 0;
}
