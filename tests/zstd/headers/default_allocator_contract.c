#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

const ZSTD_customMem* superzip_default_allocator_a(void);
const ZSTD_customMem* superzip_default_allocator_b(void);

/* Purpose: Verify one production allocator definition across C and C++ units.
 * Inputs: Linked original or repaired upstream common/error implementation.
 * Outputs: Zero only for identical storage and unchanged default callback values. */
int main(void) {
    const ZSTD_customMem* a = superzip_default_allocator_a();
    const ZSTD_customMem* b = superzip_default_allocator_b();
    return a != b || a != &ZSTD_defaultCMem || a->customAlloc != NULL || a->customFree != NULL || a->opaque != NULL;
}
