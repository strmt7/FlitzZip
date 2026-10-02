/* Public inclusion must not prevent a later static/inline API opt-in. */
#include <string.h>
#include "zstd.h"
#include "zdict.h"
#include "common/fse.h"
#include "common/xxhash.h"

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
 * Inputs: argc/argv are the standard program arguments; header identity is
 * selected by the isolated CMake fixture, not by runtime input.
 * Outputs: Produces a linkable fixture only when every contract remains valid. */
int main(int argc, char** argv) {
    if (argc < 1) {
        return 1;
    }
    (void)sizeof(ZSTD_compressionParameters);
    (void)sizeof(ZDICT_cover_params_t);
    (void)sizeof(FSE_CState_t);
    (void)sizeof(COVER_segment_t);
    (void)sizeof(&HIST_count);
    (void)HIST_WKSP_SIZE;
    return (int)(XXH64(argv[0], strlen(argv[0]), 0) & 1);
}
