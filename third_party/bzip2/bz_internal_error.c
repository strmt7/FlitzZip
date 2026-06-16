/* SuperZip link shim for libbzip2's internal assertion hook.
   The official library declares this symbol for embedding applications. */
#include <stdlib.h>

void bz_internal_error(int errcode) {
    (void)errcode;
    abort();
}
