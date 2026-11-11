# Compact Entropy Reader Build Review

The maintainer authorizes repairing defects and retaining only individually
proven false positives. This review renews the four existing alerts below in
their existing scanner categories after adding the compact entropy component
and its native tests. Complete reported files, highlighted locations, rules
and scanner versions were compared with retained raw reports and are unchanged.
The ledger retains preceding context paths and binds the new component, tests,
CMake source list and this review. It admits no other finding.

The CMake change adds one source to the existing core target and one source to
the existing native test target. Compiler flags, sanitizer definitions, allocator
selection, old sources, existing registrations and platform guards are unchanged.
The new component owns no heap buffers or asynchronous work; callers provide
its bounded model workspace. It installs no allocator, callback or test hook.
It does not register kind 14, admit archive version 12 or enable a new writer.

The component selector maps only the new reader source/header and fixed payload
header to its exact registered reader cases. Existing component mappings remain
unchanged; CMake changes still require the full driver. The offline routing
contract checks both the complete reader cohort and that broad-input boundary.

The new cases exercise independently generated fixed payloads, canonical models,
stream/state bounds, aliases and scratch ownership. Their execution and the
affected build/analysis consumers remain required; this static review does not
claim that the new cases or fresh analyses passed.

## Individual Decisions

### Alert 8348

The `malloc` at the unchanged miniz location remains confined to the explicit
fuzz/sanitizer allocator branch; normal Windows builds still select the process
heap. Its matching release/resize family and guarded definitions are unchanged.
The new C++ component neither includes this allocator header nor changes the
fuzz build source list or flags. The existing
[allocator review](miniz-sanitizer-context.md#alert-8348) remains applicable.

### Alert 632

The Lhasa stream still borrows its input handle under the public callback
lifetime contract. `LhaReaderSession` owns its input and frees its C reader and
stream in the destructor body before releasing that input. Construction failure
retains the same cleanup. The new component installs no LHA callback, and its
tests do not enter that adapter. See the unchanged
[lifetime review](lha-callback-lifetime.md#alert-632).

### Alert 8422

`require_tar_writer_path_boundary` retains its direct calls from the registered
TAR boundary test. That complete test source and its CMake membership are
unchanged. Adding another test translation unit neither removes those calls
nor changes registration or conditional compilation. See the preceding
[individual review](neutron-compaction-context.md#alert-8422).

### Alert 8431

The GDI test still initializes an unsigned local induction variable at zero,
increments it once per completed iteration and checks eight successive colors
and allocation counts. The new component does not change the loop, its callbacks,
surface provider or GDI lifetime. Existing test target definitions and registration
remain intact. The [loop review](gdi-rejection-loop.md#alert-8431) still applies
to the exact reported condition in both CPU and HIP-host categories.
