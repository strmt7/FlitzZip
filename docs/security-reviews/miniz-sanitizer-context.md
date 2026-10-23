# Miniz sanitizer allocator context

## Reviewed source

### Alert 8348

DevSkim `DS161085` identifies `malloc` at
`third_party/miniz/miniz_common.h:76:11–76:33`. The complete source remains
unchanged. This is the individually reviewed sanitizer allocator described in
[the source review](current-source-findings.md#alert-8348), rather than a
production defect or an exemption for other allocations.

The allocator uses the matching `malloc`/`realloc`/`free` family only when both
`FLITZZIP_MINIZ_FUZZ_ALLOCATOR` and
`FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` are defined. Windows product builds
use `HeapAlloc`/`HeapReAlloc`/`HeapFree`. Unsupported ordinary platforms fail
compilation. Keeping the C allocation family in the Linux sanitizer build makes
its allocations visible to AddressSanitizer.

The AR fuzz target adds the production stream reader, its name decoder and
existing core objects to the sanitizer build. It does not change the miniz
sources, allocation branches, sanitizer flags or existing targets. Its generated
GNU/BSD seed corpus and RAM-only truncation oracle expand parser qualification.
The build command contracts and real sanitizer smoke remain required; command
fixtures alone do not establish that the fuzz binaries execute correctly.

The committed disposition binds the complete allocator source, exact location,
scanner identity, current build recipes, new harness and this evidence. Raw
reports remain retained. Changed source, callers, recipes or scanner identities
expire the decision and require a new review.
