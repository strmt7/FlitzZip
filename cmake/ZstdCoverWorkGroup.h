#ifndef SUPERZIP_ZSTD_COVER_WORK_GROUP_H
#define SUPERZIP_ZSTD_COVER_WORK_GROUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "dictBuilder/cover.h"
#include "common/pool.h"

typedef struct COVER_workGroup_s COVER_workGroup;
typedef void (*COVER_contextDestructor)(void*);

/* Purpose: Own optimizer state for the complete lifetime of all workers.
 * Inputs: Requested thread count; zero or one selects inline execution.
 * Outputs: Exclusive owner or null after rollback; release through the matching function. */
COVER_workGroup* COVER_createWorkGroup(unsigned threads);

/* Purpose: Borrow the synchronized result record from its heap owner.
 * Inputs: Live group or null. Outputs: Record valid until group release, or null. */
COVER_best_t* COVER_workGroupBest(COVER_workGroup* group);

/* Purpose: Borrow the worker pool without transferring ownership.
 * Inputs: Live group or null. Outputs: Pool valid until release, or null for inline execution. */
POOL_ctx* COVER_workGroupPool(COVER_workGroup* group);

/* Purpose: Copy the completed owned dictionary into the caller's bounded output.
 * Inputs: Joined group and live writable caller extent. Outputs: Dictionary size or parameter error. */
size_t COVER_copyWorkDictionary(COVER_workGroup* group, void* destination, size_t capacity);

/* Purpose: Acquire private C context storage before initialization.
 * Inputs: Live group, positive extent, fundamental alignment and nonthrowing destruction callback.
 * Outputs: Borrowed storage or null; failure preserves the previous context. */
void* COVER_prepareWorkContext(COVER_workGroup* group, size_t bytes, size_t alignment, COVER_contextDestructor destroy);

/* Purpose: Admit successful C initialization into the context ownership tree.
 * Inputs: Live group with prepared storage whose initializer succeeded.
 * Outputs: Commits its destruction callback; failed initializers must not call this function. */
void COVER_commitWorkContext(COVER_workGroup* group);

/* Purpose: Release context only after every queued worker has returned.
 * Inputs: Live group or null. Outputs: Joins actual jobs and destroys committed context exactly once. */
void COVER_finishWorkContext(COVER_workGroup* group);

/* Purpose: Release the complete optimizer ownership tree after joining workers.
 * Inputs: Exclusive group or null. Outputs: Matching destruction of context, result state and pool. */
void COVER_releaseWorkGroup(COVER_workGroup* group);

#ifdef __cplusplus
}
#endif
#endif
