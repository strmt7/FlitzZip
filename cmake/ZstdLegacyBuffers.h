#ifndef SUPERZIP_ZSTD_LEGACY_BUFFERS_H
#define SUPERZIP_ZSTD_LEGACY_BUFFERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZBUFF_ownedBuffers_s ZBUFF_ownedBuffers;
typedef struct ZBUFF_ownedHistory_s ZBUFF_ownedHistory;
typedef struct ZBUFF_decoderOwner_s ZBUFF_decoderOwner;
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

/* Purpose: Create independent bounded history with matching ownership.
 * Inputs: A complete aligned, nonthrowing allocator pair or two null callbacks.
 * Outputs: An empty owner or null; no caller storage is retained. */
ZBUFF_ownedHistory* ZBUFF_createOwnedHistory(ZBUFF_bufferAllocator allocator);

/* Purpose: Reset history for a new decoder session without allocating.
 * Inputs: A live owner and maximum retained bytes, at most PTRDIFF_MAX.
 * Outputs: Ordinary status; successful reset retains no initialized history. */
ZBUFF_bufferResult ZBUFF_resetOwnedHistory(ZBUFF_ownedHistory* owner, size_t limit);

/* Purpose: Apply the decoded frame's history bound while preserving its newest bytes.
 * Inputs: A live owner and maximum retained bytes, at most PTRDIFF_MAX.
 * Outputs: Ordinary status; older bytes are discarded and allocations remain owned. */
ZBUFF_bufferResult ZBUFF_limitOwnedHistory(ZBUFF_ownedHistory* owner, size_t limit);

/* Purpose: Append initialized output or dictionary bytes to independent history.
 * Inputs: Complete readable source extent, offset and count; storage is distinct from the private history array.
 * Outputs: Ordinary status; allocation or geometry failures retain previous history. */
ZBUFF_bufferResult ZBUFF_appendOwnedHistory(ZBUFF_ownedHistory* owner, const void* source, size_t capacity,
                                            size_t offset, size_t count);

/* Purpose: Copy a proven initialized history segment into caller output.
 * Inputs: Complete writable destination extent, offset, distance from history end and count <= distance.
 * Outputs: Ordinary status; invalid geometry leaves output unchanged and no caller pointer is retained. */
ZBUFF_bufferResult ZBUFF_copyOwnedHistory(const ZBUFF_ownedHistory* owner, void* destination, size_t capacity,
                                          size_t offset, size_t distance, size_t count);

/* Purpose: Clone initialized history under the destination's allocation contract.
 * Inputs: Live independent owners, or the same owner for a no-op.
 * Outputs: Ordinary status; failures preserve destination history and allocator identity. */
ZBUFF_bufferResult ZBUFF_cloneOwnedHistory(ZBUFF_ownedHistory* destination, const ZBUFF_ownedHistory* source);

/* Purpose: Read actual initialized history geometry. Inputs: A live owner or null.
 * Outputs: Retained byte count, never an inferred pointer difference. */
size_t ZBUFF_ownedHistorySize(const ZBUFF_ownedHistory* owner);

/* Purpose: Release history with its matching allocator. Inputs: One owner or null.
 * Outputs: Releases the complete record and byte array exactly once. */
void ZBUFF_releaseOwnedHistory(ZBUFF_ownedHistory* owner);

/* Purpose: Construct a complete C decoder storage and history ownership tree.
 * Inputs: Matching allocator callbacks, nonzero context bytes and fundamental power-of-two alignment.
 * Outputs: A complete owner or null after rollback; the upstream C type initializes its own storage. */
ZBUFF_decoderOwner* ZBUFF_createDecoderOwner(ZBUFF_bufferAllocator allocator, size_t bytes, size_t alignment);

/* Purpose: Borrow the complete decoder allocation from its parent ownership tree.
 * Inputs: A live owner. Outputs: Aligned storage valid until that owner is released. */
void* ZBUFF_decoderStorage(const ZBUFF_decoderOwner* owner);

/* Purpose: Access independent history owned by the decoder tree.
 * Inputs: A live owner. Outputs: Its live history record, never caller-owned storage. */
ZBUFF_ownedHistory* ZBUFF_decoderHistory(const ZBUFF_decoderOwner* owner);

/* Purpose: Estimate the initial complete decoder footprint without allocation.
 * Inputs: Representable C context bytes. Outputs: Context and ownership-record bytes, or SIZE_MAX on overflow. */
size_t ZBUFF_decoderBaseSize(size_t bytes);

/* Purpose: Account for actual decoder storage, records and cached history capacity.
 * Inputs: One complete live owner or null. Outputs: Its complete allocated byte count, or zero for null. */
size_t ZBUFF_decoderOwnedSize(const ZBUFF_decoderOwner* owner);

/* Purpose: Release history and decoder storage under their original allocation contract.
 * Inputs: One complete owner or null. Outputs: Releases the complete tree exactly once. */
void ZBUFF_releaseDecoderOwner(ZBUFF_decoderOwner* owner);

#ifdef __cplusplus
}
#endif
#endif
