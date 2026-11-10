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

The fuzz build parses framework flags once into argument arrays and quotes each
array expansion. Whitespace remains the framework's argument separator; wildcard
and shell-operator characters remain data. Both allocator-selection definitions
remain explicit compiler arguments on every miniz object, and the undefined
sanitizer's alignment instrumentation is appended before parsing. Command-contract
tests check actual compiler argv, including literal wildcard and newline controls.
Real hosted sanitizer execution remains required independently of those fixtures.

CPack version policy, installer fixtures and their bounded shared MSI process
transport do not enter the product or fuzz target graph or redefine allocator
selection. Native controller tests and optional HIP profiling likewise do not
change miniz's allocation family. Scanner scope and sanitizer requirements remain
unchanged; MSI transport tests cannot qualify an allocator.
The SDK compiler-policy check now recognizes the explicit C++ argument array;
independent controls still reject unquoted flags, a missing wire implementation
and missing Release optimization. This does not alter miniz instrumentation.
Tool-lock inventory rules and their filesystem fixtures do not change miniz's
allocation family, compiler arguments or sanitizer controls. The complete
`Test-FuzzHarnessPolicy` body and the native/fuzz inputs remain unchanged when
qualifying that policy-only context update.

The ledger binds complete source, exact location, scanner identity, build/fuzz
recipes and this evidence. Raw reports remain retained. Changed bound inputs
expire admission. Current native consumers, real sanitizer controls and fresh
detector results remain separate required qualifications.

The encoded-block version regression adds only a source to the existing native
test object target. Miniz's product target, allocation wrappers, fuzz compiler
arguments, allocator-selection definitions and sanitizer controls are unchanged.
Those exact inputs were rechecked before renewing this build-context binding;
the new tests do not qualify the allocator or admit another allocation report.
