#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

/* Purpose: Observe the same production storage from a C++ caller.
 * Inputs: None. Outputs: C-linked address of the immutable allocator value. */
extern "C" const ZSTD_customMem* superzip_default_allocator_b(void) {
    return &ZSTD_defaultCMem;
}
