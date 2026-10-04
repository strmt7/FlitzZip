#ifndef SUPERZIP_ZSTD_LEGACY_BUFFERS_H
#define SUPERZIP_ZSTD_LEGACY_BUFFERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZBUFF_ownedBuffers_s ZBUFF_ownedBuffers;
typedef struct {
    void* (*allocate)(void* opaque, size_t bytes);
    void (*release)(void* opaque, void* address);
    void* opaque;
} ZBUFF_bufferAllocator;
typedef struct {
    char* input;
    size_t inputCapacity;
    char* output;
    size_t outputCapacity;
} ZBUFF_bufferView;
typedef enum { ZBUFF_buffer_ok, ZBUFF_buffer_invalid, ZBUFF_buffer_allocation_failure } ZBUFF_bufferResult;

/* Purpose: Create the exclusive buffer owner for a decoder.
 * Inputs: Both aligned, nonthrowing allocator callbacks are present, or both are null for array allocation.
 * Outputs: Returns an empty owner or null on failure; the caller owns it until release. */
ZBUFF_ownedBuffers* ZBUFF_createOwnedBuffers(ZBUFF_bufferAllocator allocator);

/* Purpose: Grow decoder storage without publishing a partially acquired pair.
 * Inputs: A live exclusive owner and validated requested byte capacities.
 * Outputs: Returns status; failure retains both old buffers and capacities. */
ZBUFF_bufferResult ZBUFF_reserveOwnedBuffers(ZBUFF_ownedBuffers* owner, size_t inputSize, size_t outputSize);

/* Purpose: Borrow actual allocation geometry from an owner.
 * Inputs: A live owner, or null during failed-construction cleanup.
 * Outputs: Returns its current buffer/capacity pairs, valid until growth or release. */
ZBUFF_bufferView ZBUFF_viewOwnedBuffers(const ZBUFF_ownedBuffers* owner);

/* Purpose: Release the owner and all arrays using their matching allocators.
 * Inputs: An owner returned by create, or null. Outputs: Releases exactly one ownership tree. */
void ZBUFF_releaseOwnedBuffers(ZBUFF_ownedBuffers* owner);

/* Purpose: Copy initialized bytes only after validating both complete geometries.
 * Inputs: Live source/destination extents, offsets and exact count; overlap is supported.
 * Outputs: Returns status; invalid geometry leaves destination unchanged, with no pointer formation. */
ZBUFF_bufferResult ZBUFF_copyBytes(void* destination, size_t destinationCapacity, size_t destinationOffset,
                                   const void* source, size_t sourceCapacity, size_t sourceOffset, size_t count);

#ifdef __cplusplus
}
#endif
#endif
