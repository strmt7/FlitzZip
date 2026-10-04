# Exact DevSkim Source Review, October 4

Current status: historical review, superseded as an acceptance mechanism by
the [source remediation redesign](security-source-remediation-redesign.md).
The maintainer subsequently requested source repairs instead of admission.
Any source report still open in fresh analysis remains blocking. The statements and hashes
below record the earlier assessment only; they do not establish current source
identity, harmlessness, remediation or security acceptance.

These 21 reports were individually reviewed against the source recorded below. The
maintainer explicitly approved these exact dispositions on October 4. The
ledger in `.github/scanner-source-reviews.csv` binds each rule, complete source
digest and line/column region. Only CRLF/LF normalization is permitted. Changed
source bytes, another location or another rule invalidate the disposition.
Raw reports remain complete; no file, directory or rule is excluded. This
approval does not resolve any other DevSkim or CodeQL report.

The HIP, allocation-fault, guard-page and independent legacy-sequence oracle checks and all six ASan targets passed for native input identity `70bc8379378fd228843e4153f700500b43facb81a0a58f09c68fdd10854633d7` at that review. The retained assessments describe that tested revision.

## DS121708: cmake/ZstdLegacyStreamV06.c, line 195

Source SHA-256: `3ed27d2dbc90a125c099d53389122d04e90a2a4e54960e124473abf098dce614`.

getFrameParams requests hSize > lhSize; the preceding short-input branch returns unless toLoad bytes are available. Header size is bounded by the legacy frame-header maximum. The full-header branch copies toLoad from a live input cursor into the remaining header span, sets lhSize to hSize, and advances the cursor by the copied byte count. Canonical partial-header tests exercise this version-specific consumer.

## DS121708: cmake/ZstdLegacyStreamV05.c, line 178

Source SHA-256: `9ef852b13bdedab43ed18f700130785daa75e3daad0446ca1760d6465124b8fd`.

getFrameParams reports incomplete initial header only when supplied input is smaller than its fixed header requirement. The copy executes only for nonzero supplied bytes and records exactly that count in hPos; canonical partial-header tests exercise recovery.

## DS121708: cmake/ZstdLegacyStreamV05.c, line 57

Source SHA-256: `9ef852b13bdedab43ed18f700130785daa75e3daad0446ca1760d6465124b8fd`.

Accumulated v0.5 header bytes are bounded by getFrameParams/headerBuffer and then copied into the successfully reserved BLOCKSIZE input buffer. Nonzero hPos selects the copy; partial-header and fault tests exercise that direct consumer.

## DS161085: cmake/ZstdLegacyStreamV06.c, line 28

Source SHA-256: `3ed27d2dbc90a125c099d53389122d04e90a2a4e54960e124473abf098dce614`.

Reserve acquires both replacement buffers before releasing old owners, checks allocation failure, frees partial replacement, and publishes matching capacities only on success. Canonical initial/growth allocation-fault tests retain old owner identity and successful retry.

## DS161085: cmake/ZstdLegacyStreamV05.c, line 28

Source SHA-256: `9ef852b13bdedab43ed18f700130785daa75e3daad0446ca1760d6465124b8fd`.

Reserve acquires both replacement buffers before releasing old owners, checks allocation failure, frees partial replacement, and publishes matching capacities only on success. Canonical initial/growth allocation-fault tests retain old owner identity and successful retry.

## DS161085: cmake/ZstdCoverSelection.c, line 13

Source SHA-256: `941c5e200b20c60df7364ffa6844dd778a57c35781a0d55a7b45bdf4a4eaf58c`.

Two allocations borrow validated builder capacity. Every NULL combination and finalizer/check failure releases acquired storage; successful ownership transfers exactly one buffer. Canonical allocation-fault and finalizer-fault tests exercise these branches.

## DS121708: cmake/ZstdLegacyLiteralsV07.c, line 108

Source SHA-256: `93ebfaa98ced1bc095f858fdb1fce54f4a89ea39d5f184de843e1de702761ffc`.

Raw-literal header supplies a bounded wire length. The helper rejects truncated payload before copying; the enclosing block decoder restricts source below its block maximum, so copied literals fit the context block buffer with wildcopy padding. This call preserves the valid raw-literal encoding; copied and borrowed paths require continued exact-source SAST.

## DS154189: tests/zstd/sanitizers/asan_control.cpp, line 24

Source SHA-256: `093a91c37d7e15165b7a8baf0ca7fb7e3168b83c0ce880ab21051b38c7709f05`.

Instrumented, standalone qualification control. malloc(16) is checked and owned by unique_ptr; literal-format printf displays one unsigned byte. --heap-overflow deliberately accesses byte 16 and must terminate under ASan, while --valid accesses byte 15. No product path links this target.

## DS161085: cmake/ZstdLegacyStreamV05.c, line 23

Source SHA-256: `9ef852b13bdedab43ed18f700130785daa75e3daad0446ca1760d6465124b8fd`.

Reserve acquires both replacement buffers before releasing old owners, checks allocation failure, frees partial replacement, and publishes matching capacities only on success. Canonical initial/growth allocation-fault tests retain old owner identity and successful retry.

## DS121708: cmake/ZstdLegacyLiteralsV06.c, line 105

Source SHA-256: `c5e59f7e34a132b520cfb8f9751ea07a842fc5dd4cadc057408f743f0ca80853`.

Raw-literal header supplies a bounded wire length. The helper rejects truncated payload before copying; the enclosing block decoder restricts source below its block maximum, so copied literals fit the context block buffer with wildcopy padding. This call preserves the valid raw-literal encoding; copied and borrowed paths require continued exact-source SAST.

## DS161085: cmake/ZstdLegacyStreamV06.c, line 23

Source SHA-256: `3ed27d2dbc90a125c099d53389122d04e90a2a4e54960e124473abf098dce614`.

Reserve acquires both replacement buffers before releasing old owners, checks allocation failure, frees partial replacement, and publishes matching capacities only on success. Canonical initial/growth allocation-fault tests retain old owner identity and successful retry.

## DS161085: cmake/ZstdCoverSelection.c, line 12

Source SHA-256: `941c5e200b20c60df7364ffa6844dd778a57c35781a0d55a7b45bdf4a4eaf58c`.

Two allocations borrow validated builder capacity. Every NULL combination and finalizer/check failure releases acquired storage; successful ownership transfers exactly one buffer. Canonical allocation-fault and finalizer-fault tests exercise these branches.

## DS161085: tests/zstd/sanitizers/asan_control.cpp, line 17

Source SHA-256: `093a91c37d7e15165b7a8baf0ca7fb7e3168b83c0ce880ab21051b38c7709f05`.

Instrumented, standalone qualification control. malloc(16) is checked and owned by unique_ptr; literal-format printf displays one unsigned byte. --heap-overflow deliberately accesses byte 16 and must terminate under ASan, while --valid accesses byte 15. No product path links this target.

## DS121708: cmake/ZstdLegacyLiteralsV05.c, line 103

Source SHA-256: `cbd12e8d36778506901228c8dc498c7483523a34a944dd06004ffba717a72703`.

Raw-literal header supplies a bounded wire length. The helper rejects truncated payload before copying; the enclosing block decoder restricts source below its block maximum, so copied literals fit the context block buffer with wildcopy padding. This call preserves the valid raw-literal encoding; copied and borrowed paths require continued exact-source SAST.

## DS121708: cmake/ZstdLegacyStreamV07.c, line 201

Source SHA-256: `140023d880afd9aad6d08935ef43e819076e8f4511516f5fe5815b9bf0f8a672`.

getFrameParams requests hSize > lhSize; the preceding short-input branch returns unless toLoad bytes are available. Header size is bounded by the legacy frame-header maximum. The full-header branch copies toLoad from a live input cursor into the remaining header span, sets lhSize to hSize, and advances the cursor by the copied byte count. Canonical partial-header tests exercise this version-specific consumer.

## DS154189: cmake/ZstdLegacyHistoryV07.c, line 40

Source SHA-256: `c24e00f8084fb464a84542f9c7503ef42fbc117476b7b92909f854b88c938f10`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.

## DS154189: cmake/ZstdLegacyHistoryV06.c, line 40

Source SHA-256: `3943dfb6dedfd13385a33a7264b88a2823e6bdf1c26a8c3aa68644d65fb6607a`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.

## DS154189: cmake/ZstdLegacyHistoryV05.c, line 40

Source SHA-256: `82e41b733bc64512e48d76a983e935ec208448c411a16a12a8a652017f7b004c`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.

## DS154189: cmake/ZstdLegacyHistoryV06.c, line 37

Source SHA-256: `3943dfb6dedfd13385a33a7264b88a2823e6bdf1c26a8c3aa68644d65fb6607a`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.

## DS154189: cmake/ZstdLegacyHistoryV07.c, line 37

Source SHA-256: `c24e00f8084fb464a84542f9c7503ef42fbc117476b7b92909f854b88c938f10`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.

## DS154189: cmake/ZstdLegacyHistoryV05.c, line 37

Source SHA-256: `82e41b733bc64512e48d76a983e935ec208448c411a16a12a8a652017f7b004c`.

Canonical sequence execution proves output/literal extents and dictionaryOffset <= dictSize before either memmove. The dictionary-only branch copies matchLength <= dictionaryOffset; the crossing branch copies dictionaryOffset then continues from the current prefix. memmove preserves the borrowed-storage overlap contract; offset/length oracle tests and ASan are required, and no primitive substitution or scanner exemption was applied.
