/* Purpose: Call the complete canonical private raw-block writer in its test translation unit.
 * Inputs: destination/source and their advertised byte extents obey the writer's public contract.
 * Outputs: Returns its unchanged size/error result; no independent writer or product API is introduced. */
size_t sz_zstd_no_compress_block(void* destination, size_t capacity, const void* source, size_t bytes,
                                 unsigned last_block) {
    return ZSTD_noCompressBlock(destination, capacity, source, bytes, last_block);
}
