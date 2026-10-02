#ifndef SUPERZIP_ZSTD_INTERCEPT_ALLOCATIONS_H
#define SUPERZIP_ZSTD_INTERCEPT_ALLOCATIONS_H

#include <stdlib.h>
#include "fault_allocator.h"

/* Interpose only the dependency translation units, after libc declarations. */
#define malloc sz_fault_malloc
#define free sz_fault_free

#endif
