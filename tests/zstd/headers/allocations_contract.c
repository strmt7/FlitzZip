/* Allocation inclusion owns the complete private dependency requirement. */
#include "common/allocations.h"
#include "common/allocations.h"
#include "zstd.h"
#include "common/allocations.h"

/* Purpose: Require private allocation helpers on first/repeated inclusion without other pre-enabled headers.
 * Inputs: The compiler selects C or C++; the default allocator owns one byte.
 * Outputs: Produces a linkable contract retaining static API declarations and balanced ownership. */
int main(void) {
    void* memory = ZSTD_customCalloc(1, ZSTD_defaultCMem);
    if (memory == NULL) {
        return 1;
    }
    ZSTD_customFree(memory, ZSTD_defaultCMem);
    return 0;
}
