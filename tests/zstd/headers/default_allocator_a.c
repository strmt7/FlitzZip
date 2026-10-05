#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

/* Purpose: Observe production default allocator storage from a C caller.
 * Inputs: None. Outputs: Address of the declared immutable allocator value. */
const ZSTD_customMem* superzip_default_allocator_a(void) {
    return &ZSTD_defaultCMem;
}
