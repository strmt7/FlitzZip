/* Synchronization search responsibilities for pinned Zstandard 1.5.7.
 * The enclosing generated source retains its upstream license notice. */

typedef struct {
    const BYTE* previous;
    U64 hash;
    size_t position;
} ZSTDMT_SynchronizationSearch;

/* Purpose: Initialize the rolling window at the first eligible synchronization byte.
 * Inputs: mtctx owns the retained prefix; istart borrows the current input; search receives local state.
 * Outputs: Returns one when the retained window is already a synchronization point, otherwise zero. */
static int ZSTDMT_initializeSynchronizationSearch(const ZSTDMT_CCtx* mtctx, const BYTE* istart,
                                                  ZSTDMT_SynchronizationSearch* search) {
    U64 const hitMask = mtctx->rsync.hitMask;
    /* Initialize the loop variables. */
    if (mtctx->inBuff.filled < RSYNC_MIN_BLOCK_SIZE) {
        /* We don't need to scan the first RSYNC_MIN_BLOCK_SIZE positions
         * because they can't possibly be a sync point. So we can start
         * part way through the input buffer.
         */
        search->position = RSYNC_MIN_BLOCK_SIZE - mtctx->inBuff.filled;
        if (search->position >= RSYNC_LENGTH) {
            search->previous = istart + search->position - RSYNC_LENGTH;
            search->hash = ZSTD_rollingHash_compute(search->previous, RSYNC_LENGTH);
        } else {
            assert(mtctx->inBuff.filled >= RSYNC_LENGTH);
            search->previous = (BYTE const*)mtctx->inBuff.buffer.start + mtctx->inBuff.filled - RSYNC_LENGTH;
            search->hash =
                ZSTD_rollingHash_compute(search->previous + search->position, (RSYNC_LENGTH - search->position));
            search->hash = ZSTD_rollingHash_append(search->hash, istart, search->position);
        }
    } else {
        /* We have enough bytes buffered to initialize the rolling hash,
         * and have processed enough bytes to find a sync point.
         * Start scanning at the beginning of the input.
         */
        assert(mtctx->inBuff.filled >= RSYNC_MIN_BLOCK_SIZE);
        assert(RSYNC_MIN_BLOCK_SIZE >= RSYNC_LENGTH);
        search->position = 0;
        search->previous = (BYTE const*)mtctx->inBuff.buffer.start + mtctx->inBuff.filled - RSYNC_LENGTH;
        search->hash = ZSTD_rollingHash_compute(search->previous, RSYNC_LENGTH);
        if ((search->hash & hitMask) == hitMask) {
            /* We're already at a sync point so don't load any more until
             * we're able to flush this sync point.
             * This likely happened because the job table was full so we
             * couldn't add our job.
             */
            return 1;
        }
    }
    return 0;
}

/* Purpose: Roll the initialized window and select a synchronization cut within the admitted load extent.
 * Inputs: mtctx owns immutable rsync parameters; search is initialized; istart borrows the exact input extent.
 * Outputs: Returns bytes to load and a flush decision; processedEnd records the window's consumed extent. */
static SyncPoint ZSTDMT_scanSynchronizationWindow(const ZSTDMT_CCtx* mtctx, const BYTE* istart,
                                                  const ZSTDMT_SynchronizationSearch* search, size_t toLoad) {
    U64 const primePower = mtctx->rsync.primePower;
    U64 const hitMask = mtctx->rsync.hitMask;
    const BYTE* const prev = search->previous;
    U64 hash = search->hash;
    size_t pos = search->position;
    size_t processedEnd = pos;
    SyncPoint syncPoint = {toLoad, 0};
    /* Starting with the hash of the previous RSYNC_LENGTH bytes, roll
     * through the input. If we hit a synchronization point, then cut the
     * job off, and tell the compressor to flush the job. Otherwise, load
     * all the bytes and continue as normal.
     * If we go too long without a synchronization point (targetSectionSize)
     * then a block will be emitted anyways, but this is okay, since if we
     * are already synchronized we will remain synchronized.
     */
    assert(pos < RSYNC_LENGTH || ZSTD_rollingHash_compute(istart + pos - RSYNC_LENGTH, RSYNC_LENGTH) == hash);
    for (; pos < syncPoint.toLoad; ++pos) {
        BYTE const toRemove = pos < RSYNC_LENGTH ? prev[pos] : istart[pos - RSYNC_LENGTH];
        /* Check the complete rolling window at entry and exit rather than recomputing it per byte. */
        hash = ZSTD_rollingHash_rotate(hash, toRemove, istart[pos], primePower);
        processedEnd = pos + 1;
        assert(mtctx->inBuff.filled + pos >= RSYNC_MIN_BLOCK_SIZE);
        if ((hash & hitMask) == hitMask) {
            syncPoint.toLoad = processedEnd;
            syncPoint.flush = 1;
            break;
        }
    }
    assert(processedEnd < RSYNC_LENGTH ||
           ZSTD_rollingHash_compute(istart + processedEnd - RSYNC_LENGTH, RSYNC_LENGTH) == hash);
    return syncPoint;
}

/* Purpose: Route eligible multiworker input through rolling-window initialization and cut selection.
 * Inputs: mtctx owns initialized buffers and job parameters; input borrows a validated public extent.
 * Outputs: Returns a bounded load/flush decision, retaining minimum-block and buffered-window behavior. */
static SyncPoint findSynchronizationPoint(ZSTDMT_CCtx const* mtctx, ZSTD_inBuffer const input) {
    const BYTE* const source = (const BYTE*)input.src;
    const BYTE* const istart = input.pos != 0 ? source + input.pos : source;
    SyncPoint syncPoint;
    ZSTDMT_SynchronizationSearch search;
    syncPoint.toLoad = MIN(input.size - input.pos, mtctx->targetSectionSize - mtctx->inBuff.filled);
    syncPoint.flush = 0;
    if (!mtctx->params.rsyncable)
        /* Rsync is disabled. */
        return syncPoint;
    if (mtctx->inBuff.filled + input.size - input.pos < RSYNC_MIN_BLOCK_SIZE)
        /* We don't emit synchronization points if it would produce too small blocks.
         * We don't have enough input to find a synchronization point, so don't look.
         */
        return syncPoint;
    if (mtctx->inBuff.filled + syncPoint.toLoad < RSYNC_LENGTH)
        /* Not enough to compute the hash.
         * We will miss any synchronization points in this RSYNC_LENGTH byte
         * window. However, since it depends only in the internal buffers, if the
         * state is already synchronized, we will remain synchronized.
         * Additionally, the probability that we miss a synchronization point is
         * low: RSYNC_LENGTH / targetSectionSize.
         */
        return syncPoint;
    if (ZSTDMT_initializeSynchronizationSearch(mtctx, istart, &search)) {
        syncPoint.toLoad = 0;
        syncPoint.flush = 1;
        return syncPoint;
    }
    return ZSTDMT_scanSynchronizationWindow(mtctx, istart, &search, syncPoint.toLoad);
}
