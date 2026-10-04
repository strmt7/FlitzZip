#include "fault_allocator.h"
#include "legacy/zstd_legacy.h"
#include "common/allocations.h"

size_t sz_legacy_v05_sequence(void*, size_t, size_t, const void*, size_t, const void*, size_t, size_t, size_t, size_t);
size_t sz_legacy_v06_sequence(void*, size_t, size_t, const void*, size_t, const void*, size_t, size_t, size_t, size_t);
size_t sz_legacy_v07_sequence(void*, size_t, size_t, const void*, size_t, const void*, size_t, size_t, size_t, size_t);

/* Purpose: Dispatch explicit sequence extents to the complete canonical helper.
 * Inputs: version is 5-7; borrowed buffer extents, prefix and lengths obey the probe contract in fault_allocator.h.
 * Outputs: Returns native bytes/error identity without alternate decoding or rewriting the result. */
size_t sz_legacy_sequence(unsigned version, void* destination, size_t capacity, size_t prefix, const void* literals,
                          size_t literal_bytes, const void* dictionary, size_t dictionary_bytes, size_t literal_length,
                          size_t match_length, size_t offset) {
    switch (version) {
    case 5:
        return sz_legacy_v05_sequence(destination, capacity, prefix, literals, literal_bytes, dictionary,
                                      dictionary_bytes, literal_length, match_length, offset);
    case 6:
        return sz_legacy_v06_sequence(destination, capacity, prefix, literals, literal_bytes, dictionary,
                                      dictionary_bytes, literal_length, match_length, offset);
    case 7:
        return sz_legacy_v07_sequence(destination, capacity, prefix, literals, literal_bytes, dictionary,
                                      dictionary_bytes, literal_length, match_length, offset);
    default:
        return ERROR(GENERIC);
    }
}

/* Purpose: Classify a canonical native result; result is bytes/error, returns the unmodified error predicate. */
int sz_legacy_result_is_error(size_t result) {
    return ERR_isError(result) != 0;
}

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

int sz_legacy_v05_buffers_consistent(const void* context);
int sz_legacy_v06_buffers_consistent(const void* context);
int sz_legacy_v07_buffers_consistent(const void* context);
sz_legacy_buffer_state sz_legacy_v05_get_buffer_state(const void* context);
sz_legacy_buffer_state sz_legacy_v06_get_buffer_state(const void* context);
sz_legacy_buffer_state sz_legacy_v07_get_buffer_state(const void* context);

/* Purpose: Dispatch a read-only snapshot of production buffer ownership.
 * Inputs: context is live for version 5-7. Outputs: Returns borrowed owners/capacities or an empty unsupported state.
 */
sz_legacy_buffer_state sz_legacy_get_buffer_state(const void* context, unsigned version) {
    switch (version) {
    case 5:
        return sz_legacy_v05_get_buffer_state(context);
    case 6:
        return sz_legacy_v06_get_buffer_state(context);
    case 7:
        return sz_legacy_v07_get_buffer_state(context);
    default: {
        sz_legacy_buffer_state empty = {NULL, 0, NULL, 0};
        return empty;
    }
    }
}

/* Purpose: Dispatch a read-only buffer ownership probe to the exact production context type.
 * Inputs: context is live and version is 5-7. Outputs: Returns zero for inconsistent storage or unsupported versions.
 */
int sz_legacy_buffers_consistent(const void* context, unsigned version) {
    switch (version) {
    case 5:
        return sz_legacy_v05_buffers_consistent(context);
    case 6:
        return sz_legacy_v06_buffers_consistent(context);
    case 7:
        return sz_legacy_v07_buffers_consistent(context);
    default:
        return 0;
    }
}

/* Purpose: Run a bounded buffered decode through its production version-specific API.
 * Inputs: context is initialized; callers own source/output storage and byte counters, plus writable hint.
 * Outputs: Returns the error identity while retaining exact native consumption and hint behavior. */
int sz_legacy_decode(void* context, unsigned version, void* destination, size_t* destination_bytes, const void* source,
                     size_t* source_bytes, size_t* hint) {
    switch (version) {
    case 5:
        *hint =
            ZBUFFv05_decompressContinue((ZBUFFv05_DCtx*)context, destination, destination_bytes, source, source_bytes);
        break;
    case 6:
        *hint =
            ZBUFFv06_decompressContinue((ZBUFFv06_DCtx*)context, destination, destination_bytes, source, source_bytes);
        break;
    case 7:
        *hint =
            ZBUFFv07_decompressContinue((ZBUFFv07_DCtx*)context, destination, destination_bytes, source, source_bytes);
        break;
    default:
        return 1;
    }
    return ERR_isError(*hint) != 0;
}
