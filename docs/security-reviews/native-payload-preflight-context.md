# Native payload preflight build-context review

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” applies separately to the incidents below.
The changed existing context is `CMakeLists.txt`: one source is added to the
existing native test object target. Reported sources, coordinates, scanners,
query configuration, compiler definitions, link policy and registration macro
are unchanged. This is a review of the new context, not inherited approval.

The Windows HIP launcher also isolates command search before invoking the
selected Visual Studio initialization script. It starts from OS-known command
directories, discards stale preinitialization PATH state and disables CMD
AutoRun. The explicit SDK executable, requested MSVC selection, compiler
arguments, source graph and allocator definitions are preserved. Caller
environment restoration is exercised on both success and exception paths.
This does not alter the Linux fuzz compiler environment. The launcher,
environment helper, regression and toolchain guide are bound as additional
producer context; native execution still requires fresh qualification.

The isolated Zstandard sanitizer harness now redetects its compiler, binds the
matching runtime through a build dependency and checks compiler/runtime hashes
before execution. A CTest fixture blocks the three native consumers if the
instrumentation control fails. A separate harmless probe reads the configured
test inventory and verifies that failed setup prevents dependent execution.
These changes affect qualification tooling, not product or fuzz allocator
selection. Both automated CTest drivers use noninteractive error reporting;
failures and diagnostics remain blocking. These harness sources are bound as
additional review context without admitting any new source finding.

The local worker-pool test sources now compile in their own object target,
with their own function visibility and shared allocator imports. The
executable still imports the product APIs, and the isolated sanitizer harness
instruments the new object target. MSVC treats inconsistent DLL linkage as an
error for this owner. This separates definitions from imports without changing
the pool implementation or allocator substitutions.

The shared allocator object uses the DLL's data visibility independently of
the overridable advanced-function visibility. The local pool target retains
DLL imports for that object while owning its function definitions. A C client
compiled with this mixed interface must observe the same object address as the
ordinary C++ DLL consumer. The header transformation and cache migration admit
only complete exact source identities; the allocator's symbol, type, values
and production linkage remain unchanged.

`test_archive_payload_preflight.cpp` uses independent small composition frames,
public archive operations and the existing registration mechanism. It adds no
production export, allocator selection, callback bridge, GDI operation or
scanner setting. Its only static initialization registers the test functions.
The shared fixture-directory owner preserves the prior writer tests' cleanup
contract; it neither registers product work nor changes archive callback owners.
The ledger binds these complete test sources and this review in addition to
every previously required context path.

The source delta does not change the reported local invariants below. Original
reports remain retained. Candidate build, direct regression execution and fresh
published-SHA analysis remain separate requirements; this review claims none
of those results, device coverage or performance measurements.

## Individual decisions

### Alert 8348

DevSkim `DS161085` reports the intentional sanitizer-visible allocator in
`third_party/miniz/miniz_common.h`, SHA-256
`9f96c0dc0d3df04f2972e788c17a3f64c762f5d02c35c5841258607587239687`.
The product Windows heap path and the two explicit fuzz allocator definitions
are unchanged. Adding the test source does not enter or alter the fuzz object
graph. The [allocator review](miniz-sanitizer-context.md#alert-8348) remains
applicable to this exact operation; it admits no new allocation finding.

### Alert 632

CodeQL `cpp/stack-address-escape` reports the borrowed callback context in
`third_party/lhasa/lib/lha_input_stream.c`, SHA-256
`9a0c7fdcfcc16d1cd2fcd64701f104f999fe2553749ac788158719445a20aa8d`.
The complete stream implementation and LHA caller closure are unchanged.
The new fixtures do not instantiate or retain an LHA callback context. Reader,
stream and context destruction retain their reviewed order; see the
[ownership review](lha-callback-lifetime.md#alert-632).

### Alert 8422

CodeQL `cpp/unused-static-function` reports the helper in
`tests/cpp/test_tar_compat.cpp`, SHA-256
`c7bca787038c744f55d7ec6a7909c51a40e8f8155220d2b1c47e74a28f1daf91`.
Its direct call, registered caller, runner and eight independent fixtures
remain unchanged. The new CMake source is additive and does not replace or
condition that source. See the [reachability review](codeql-tar-reader-context.md#alert-8422).

### Alert 8431

CodeQL `cpp/constant-comparison` reports the local induction variable in
`tests/cpp/test_gdi_back_buffer.cpp`, SHA-256
`77d2801332eb2f9c6720e0810fbe7d665eb9bc3648eb621eba77652ab3ff5c67`.
Its unsigned zero-through-seven loop, distinct allocation/color assertions and
ninth allocation/release are unchanged. The new source introduces no callback
or reference to this variable. See the [induction review](gdi-rejection-loop.md#alert-8431).
