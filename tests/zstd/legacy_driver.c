#include "fault_allocator.h"
#include "legacy/zstd_legacy.h"

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
