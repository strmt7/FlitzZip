#ifndef SUPERZIP_ZSTD_FAULT_ALLOCATOR_H
#define SUPERZIP_ZSTD_FAULT_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Purpose: Configure a serial allocation-failure fixture without abandoning live objects.
 * Inputs: fail_on is a one-based allocation attempt, or zero to allow every allocation.
 * Outputs: Returns one on reset, zero if allocations remain; clears attempt and invalid-free counters. */
int sz_fault_reset(size_t fail_on);

/* Purpose: Select a future failure without losing allocation ownership.
 * Inputs: additional_attempts is a relative attempt count, or zero to disable injection.
 * Outputs: Returns one on configuration, zero on counter overflow; preserves all live allocations. */
int sz_fault_fail_after(size_t additional_attempts);

/* Purpose: Allocate bytes for the production legacy decoder under deterministic fault injection.
 * Inputs: bytes is the requested size; tests are serial and own all returned allocations.
 * Outputs: Returns tracked storage or NULL at the selected attempt or a real allocation failure. */
void* sz_fault_malloc(size_t bytes);

/* Purpose: Release tracked legacy-decoder storage and detect ownership violations safely.
 * Inputs: address is NULL or an allocation returned by sz_fault_malloc.
 * Outputs: Frees live storage; records, but does not repeat, invalid or double frees. */
void sz_fault_free(void* address);

/* Purpose: Inspect fixture ownership; no inputs, returns the live allocation count. */
size_t sz_fault_live_allocations(void);

/* Purpose: Inspect allocation ordering. Inputs: None. Outputs: Attempts since the last serial reset. */
size_t sz_fault_allocation_attempts(void);

/* Purpose: Inspect fixture release failures; no inputs, returns the invalid-free count. */
size_t sz_fault_invalid_frees(void);

/* Purpose: Construct an actual upstream buffered decoder with intercepted allocation.
 * Inputs: version is a supported legacy version, 5, 6 or 7.
 * Outputs: Returns the caller-owned context or NULL; no decoding or alternate implementation. */
void* sz_legacy_create(unsigned version);

/* Purpose: Free an actual upstream buffered decoder.
 * Inputs: context is NULL or belongs to version; version is 5, 6 or 7.
 * Outputs: Releases all decoder allocations through the interposed allocator. */
void sz_legacy_free(void* context, unsigned version);

/* Purpose: Exercise the production legacy context transition and initialization boundary.
 * Inputs: context is the current owner, previous/version identify its transition, dictionary is borrowed for this call.
 * Outputs: Returns one on error, zero on success; mutates context exactly as the upstream initializer does. */
int sz_legacy_initialize(void** context, unsigned previous, unsigned version, const void* dictionary, size_t bytes);

/* Purpose: Inspect owned buffer invariants after allocation failure without attempting an unsafe retry.
 * Inputs: context is a live decoder for version 5-7. Outputs: Returns one when published capacities have storage. */
int sz_legacy_buffers_consistent(const void* context, unsigned version);

typedef struct {
    const void* input;
    size_t input_capacity;
    const void* output;
    size_t output_capacity;
} sz_legacy_buffer_state;

/* Purpose: Observe exact production buffer owners and capacities across a failed growth operation.
 * Inputs: context is a live decoder for version 5-7. Outputs: Returns borrowed identities; transfers no ownership. */
sz_legacy_buffer_state sz_legacy_get_buffer_state(const void* context, unsigned version);

/* Purpose: Exercise the exact production buffered streaming entry point under allocation faults.
 * Inputs: context is initialized; source/output extents are writable counters, hint receives the native result.
 * Outputs: Returns one on error, zero on success; counters retain the production API's contract. */
int sz_legacy_decode(void* context, unsigned version, void* destination, size_t* destination_bytes, const void* source,
                     size_t* source_bytes, size_t* hint);

/* Purpose: Exercise the production zero-allocation helper with optional fault callbacks.
 * Inputs: bytes is the bounded test extent; custom selects the tracked allocator instead of the standard allocator.
 * Outputs: Returns zeroed owned storage or NULL; successful custom storage was nonzero before initialization. */
void* sz_custom_calloc(size_t bytes, int custom);

/* Purpose: Release helper storage through the same allocator identity.
 * Inputs: address is NULL or a helper result; custom matches its acquisition.
 * Outputs: Frees the allocation or does nothing for NULL; tracked releases retain ownership diagnostics. */
void sz_custom_free(void* address, int custom);

/* Purpose: Execute a sequence through the canonical version-specific helper.
 * Inputs: version is 5-7; buffers own their stated extents, prefix <= capacity, literals retain wildcopy padding.
 * Outputs: Returns native bytes/error identity; malformed extents must leave output unchanged. */
size_t sz_legacy_sequence(unsigned version, void* destination, size_t capacity, size_t prefix, const void* literals,
                          size_t literal_bytes, const void* dictionary, size_t dictionary_bytes, size_t literal_length,
                          size_t match_length, size_t offset);

/* Purpose: Classify a native decoder result; result is bytes or an encoded error, returns one only for an error. */
int sz_legacy_result_is_error(size_t result);

/* Purpose: Verify malformed-block history handling in canonical v0.7; no inputs, returns one for preserved history. */
int sz_legacy_v07_block_error_history(void);

#ifdef __cplusplus
}
#endif

#endif
