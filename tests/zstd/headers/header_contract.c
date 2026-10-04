/* Public inclusion must not prevent a later static/inline API opt-in. */
#include "zstd.h"
#include "zdict.h"
#include "common/fse.h"
#include "common/xxhash.h"

/* Optional state must remain available before a later inline namespace. */
#define XXH_STATIC_LINKING_ONLY
#include "common/xxhash.h"
typedef char hash_state_was_complete[(sizeof(XXH32_state_t) > 0) ? 1 : -1];

#define ZSTD_STATIC_LINKING_ONLY
#define ZDICT_STATIC_LINKING_ONLY
#define FSE_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "zstd.h"
#include "zdict.h"
#include "common/fse.h"
#include "common/xxhash.h"
#include "zstd.h"
#include "zdict.h"
#include "common/fse.h"
#include "common/xxhash.h"
#include "common/allocations.h"
#include "common/allocations.h"
#include "dictBuilder/cover.h"
#include "compress/hist.h"

#if SUPERZIP_REPEAT_PRIVATE_HEADERS
#include "dictBuilder/cover.h"
#include "compress/hist.h"
#endif

/* Purpose: Require complete private API types and inline hash linkage.
 * Inputs: A fixed byte array with an explicit extent; header identity is
 * selected by the isolated CMake fixture, not by runtime input.
 * Outputs: Produces a linkable fixture only when every contract remains valid. */
int main(void) {
    const unsigned char input[] = {0, 1, 127, 128, 254, 255, 42, 17, 0};
    (void)sizeof(ZSTD_compressionParameters);
    (void)sizeof(ZDICT_cover_params_t);
    (void)sizeof(FSE_CState_t);
    (void)sizeof(COVER_segment_t);
    (void)sizeof(&HIST_count);
    (void)HIST_WKSP_SIZE;
    return (int)(XXH64(input, sizeof(input), 0) & 1);
}
