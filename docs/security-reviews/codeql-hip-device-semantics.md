# CodeQL HIP Device-Semantics Review

## Scope and evidence

The nine reports below are CodeQL 2.27.3 `cpp/unused-static-variable`
diagnostics from the complete `hip-host` analysis of commit
`71202a1e92a6e6989056cf80377ceec3d50a4407`. The original reports remain
in GitHub code scanning. These decisions are limited to their exact source,
coordinates, callers, tests, analysis category and scanner configuration in
the committed review ledger. A new location, source version, query version or
unreviewed alert remains blocking.

The [CodeQL support matrix](https://codeql.github.com/docs/codeql-overview/supported-languages-and-frameworks/)
lists C/C++ Clang extensions through Clang 23; the qualified ROCm 10.1 HIP
compiler is Clang 24. The
[HIP language reference](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_cpp_language_extensions.html)
defines `__global__` as a kernel function qualifier and `__constant__` as a
device-memory qualifier. Both official pages were read with the repository's
isolated Crawl4AI tool on 25 October 2026 (HTTP 200, substantive text); native
results are retained locally. CodeQL's exact parser failure mechanism is not
established, so this review relies on the directly inspected code, not on an
assumption that every HIP diagnostic is harmless.

The ROCm 10.1 Release build and all seven native CTest suites passed with
required HIP. GPU prefix packing, independent CPU readback, malformed input
rejection and sparse gathering have direct runtime tests. These results do
not imply full device instruction coverage or universal correctness.

## Individual decisions

### Alert 703

At `src/gpu/hip_codec.hip.cpp:15`, CodeQL identifies `__constant__` as a
static variable. It is the HIP qualifier on `kDeviceCrc32Table`, which is read
at five later sites in the same translation unit while computing decoded CRCs.
The table is not an unused variable; removing it would change GPU integrity
checking. The native GPU roundtrip and corruption-rejection tests exercise the
relevant decode and CRC routes.

### Alert 8452

At `src/gpu/hip_codec.hip.cpp:446`, `block_end` is read in the immediately
following fill, literal and pattern branches to bound each CRC loop. The
reported value is neither static nor unused. It prevents reading beyond a
decoded block; deleting it would weaken the device parser.

### Alert 8453

At `src/gpu/hip_codec_static_prefix.hip.cpp:117`, `word_count` bounds the
immediately following scratch-word loop. The value limits atomic writes to
the encoded segment. Required-HIP static prefix packing and readback run in
the GPU prefix suite.

### Alert 8454

At `src/gpu/hip_codec_static_prefix.hip.cpp:116`, `word_base` is read in
`atomicOr(segment_words + word_base + word, scratch[word])` within the same
kernel. It selects the destination word for the current thread. Required-HIP
static prefix packing and readback run in the GPU prefix suite.

### Alert 8455

At `src/gpu/hip_sparse_pattern.hip.cpp:11`, `__global__` qualifies
`count_sparse_positions_kernel`; it is not a static variable. The kernel is
registered in `bind_sparse_pattern_kernels`, launched from
`src/gpu/hip_sparse_pattern.cpp`, and tested with real device mismatch counts
and guarded sparse gathering in `test_gpu_kernel_rejection.cpp`.

### Alert 8456

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:123`, `word_count` bounds the
immediately following scratch-word loop. It limits atomic writes for an
adaptive prefix segment. Required-HIP adaptive prefix packing and readback run
in the GPU prefix suite.

### Alert 8457

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:122`, `word_base` is read by
the immediately following `atomicOr` destination expression. It selects the
thread's encoded word range. Required-HIP adaptive prefix packing and
readback run in the GPU prefix suite.

### Alert 8458

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:113`, `code` is shifted and
masked in the immediately following bit loop. The bits populate the encoded
scratch words; deleting it would corrupt the stream. The GPU prefix suite
checks byte-exact readback and independent reference packing.

### Alert 8459

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:112`, `width` bounds the
immediately following bit loop. The loop packs exactly the codebook width for
each symbol; deleting it would corrupt the stream. The GPU prefix suite checks
byte-exact readback and malformed-codeword rejection.
