#include "fault_allocator.h"
#include "legacy/zstd_legacy.h"
#include "common/allocations.h"

/* Purpose: Provide nonzero custom storage; opaque is unused, bytes is the test extent, returns tracked storage or NULL.
 */
static void* sz_custom_allocate(void* opaque, size_t bytes) {
    void* address;
    (void)opaque;
    address = sz_fault_malloc(bytes);
    if (address != NULL) {
        memset(address, 0xA5, bytes);
    }
    return address;
}

/* Purpose: Return tracked custom storage; opaque is unused, address is owned or NULL, no return value. */
static void sz_custom_release(void* opaque, void* address) {
    (void)opaque;
    sz_fault_free(address);
}

/* Purpose: Test production zero-allocation semantics; bytes and custom select the plan, returns storage or NULL. */
void* sz_custom_calloc(size_t bytes, int custom) {
    ZSTD_customMem memory = ZSTD_defaultCMem;
    if (custom) {
        memory.customAlloc = sz_custom_allocate;
        memory.customFree = sz_custom_release;
    }
    return ZSTD_customCalloc(bytes, memory);
}

/* Purpose: Test production release semantics; address and custom retain allocator identity, releases storage. */
void sz_custom_free(void* address, int custom) {
    ZSTD_customMem memory = ZSTD_defaultCMem;
    if (custom) {
        memory.customAlloc = sz_custom_allocate;
        memory.customFree = sz_custom_release;
    }
    ZSTD_customFree(address, memory);
}

/* Purpose: Create the upstream decoder for version 5, 6 or 7; returns an owned pointer or NULL. */
void* sz_legacy_create(unsigned version) {
    switch (version) {
    case 5:
        return ZBUFFv05_createDCtx();
    case 6:
        return ZBUFFv06_createDCtx();
    case 7:
        return ZBUFFv07_createDCtx();
    default:
        return NULL;
    }
}

/* Purpose: Release context through its upstream version-specific owner; accepts NULL, has no return value. */
void sz_legacy_free(void* context, unsigned version) {
    (void)ZSTD_freeLegacyStreamContext(context, version);
}

/* Purpose: Initialize the actual legacy transition; caller supplies ownership and dictionary, returns error identity.
 */
int sz_legacy_initialize(void** context, unsigned previous, unsigned version, const void* dictionary, size_t bytes) {
    return ERR_isError(ZSTD_initLegacyStream(context, previous, version, dictionary, bytes)) != 0;
}
