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

#ifdef __cplusplus
}
#endif

#endif
