# Miniz sanitizer allocator context

## Reviewed source

### Alert 8348

DevSkim `DS161085` identifies `malloc` at
`third_party/miniz/miniz_common.h:76:11–76:33`. The complete reported source is
unchanged. The [individual source review](current-source-findings.md#alert-8348)
explains the sanitizer instrumentation requirement; this decision admits no
other allocation or finding.

Windows product builds use the matching Windows heap family. The C allocation
family is compiled only when both `FLITZZIP_MINIZ_FUZZ_ALLOCATOR` and
`FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` are defined. Unsupported ordinary
platforms fail compilation. Preserving `malloc`/`realloc`/`free` in the Linux
fuzz build makes those allocations visible to AddressSanitizer.

The current CMake edit adds only CPack version/configuration metadata. Its WIX
policy checks numeric version ordering and changes no native target, compiler
flag, allocator definition, miniz caller or sanitizer recipe. The separate
installer fixture project does not enter the product or fuzz target graph.
Native controller tests and optional HIP profiling likewise do not redefine
miniz's allocator configuration.

The source-policy change checks the shared MSI process module and its release
consumers instead of an inline workflow helper. It preserves the bounded timeout
and adds mutation controls for missing native transport and qualification
guards. The allocator/fuzzer instrumentation policy, scanner scope and sanitizer
requirements remain unchanged. MSI transport tests do not qualify an allocator.

The ledger binds complete source, exact location, scanner identity, build/fuzz
recipes and this evidence. Raw reports remain retained. Changed bound inputs
expire admission. Current native consumers, real sanitizer controls and fresh
detector results remain separate required qualifications.
