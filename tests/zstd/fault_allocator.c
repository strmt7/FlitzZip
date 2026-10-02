#include "fault_allocator.h"

#include <stdlib.h>
#include <stdint.h>

static void* live_addresses[32];
static size_t live_count;
static size_t allocation_attempts;
static size_t fail_on_attempt;
static size_t invalid_frees;

/* Purpose: Reset serial fault state; fail_on selects an allocation, returns zero if live storage remains. */
int sz_fault_reset(size_t fail_on) {
    if (live_count != 0) {
        return 0;
    }
    allocation_attempts = 0;
    fail_on_attempt = fail_on;
    invalid_frees = 0;
    return 1;
}

/* Purpose: Configure a relative failure without discarding ownership; returns zero if the counter would overflow. */
int sz_fault_fail_after(size_t additional_attempts) {
    if (additional_attempts > SIZE_MAX - allocation_attempts) {
        return 0;
    }
    fail_on_attempt = additional_attempts == 0 ? 0 : allocation_attempts + additional_attempts;
    return 1;
}

/* Purpose: Allocate tracked decoder bytes; bytes is bounded by the fixture, returns NULL on injected/real failure. */
void* sz_fault_malloc(size_t bytes) {
    size_t index;
    void* address;
    ++allocation_attempts;
    if (allocation_attempts == fail_on_attempt) {
        return NULL;
    }
    for (index = 0; index < sizeof(live_addresses) / sizeof(live_addresses[0]); ++index) {
        if (live_addresses[index] == NULL) {
            address = malloc(bytes);
            if (address != NULL) {
                live_addresses[index] = address;
                ++live_count;
            }
            return address;
        }
    }
    return NULL;
}

/* Purpose: Free tracked address once; NULL is accepted and invalid releases increment a diagnostic counter. */
void sz_fault_free(void* address) {
    size_t index;
    if (address == NULL) {
        return;
    }
    for (index = 0; index < sizeof(live_addresses) / sizeof(live_addresses[0]); ++index) {
        if (live_addresses[index] == address) {
            live_addresses[index] = NULL;
            --live_count;
            free(address);
            return;
        }
    }
    ++invalid_frees;
}

/* Purpose: Report fixture ownership; no inputs, returns the live allocation count. */
size_t sz_fault_live_allocations(void) {
    return live_count;
}

/* Purpose: Report invalid releases; no inputs, returns the invalid-free count. */
size_t sz_fault_invalid_frees(void) {
    return invalid_frees;
}
