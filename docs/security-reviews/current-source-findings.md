# Current Individual Source-Finding Reviews

The maintainer explicitly authorized: "Fix defects; allow exact proven
false-positive dispositions." These decisions apply only to the individual
reports below. They do not revive historical approval cohorts or assert that
all source code is defect-free. The repaired PPMd empty-output defect remains
a production repair with its own regression.

The [committed ledger](../../.github/scanner-false-positive-reviews.csv) binds
each decision to its repository, alert, rule, scanner version/category, exact
location, complete source and relevant caller/test/configuration bytes. The
evidence document itself is part of the binding. Any mismatch leaves the
report unresolved; a new alert never inherits a decision by rule or path.
The post-push audit retains raw open and reproduced dismissed reports before
applying these decisions. GitHub dismissal alone is never sufficient.

Native consumer evidence is the completed HIP-enabled qualification of
f6014cb0dcd0380c13b1e7356fe7c3f7f7bbe7f7, including all seven CTest suites.
These source files are unchanged by this review. The full CPU and HIP-host
CodeQL analyses of that revision both succeeded. No scanner, query, test or
coverage path is removed. Future changed source still runs pinned preflight;
renewing a disposition requires another substantive review of the changed bytes.

## Individual Decisions

### Alert 819

**Fixed DROPFILES header copy.** `DS121708` at `src/app/drop_payload.hpp:162`.

The source snapshot has already been required to contain at least `sizeof(DropFilesHeader)` bytes. The destination is exactly that trivially copyable header type. The fixed-size copy is followed by offset, alignment, terminator and payload-budget validation; malformed shell-drop tests remain enabled.

### Alert 820

**Locked shell payload snapshot.** `DS121708` at `src/app/drop_payload.hpp:159`.

`GlobalSize` supplies the bounded allocation size, `GlobalLockView` holds the source lock through the copy, and the destination vector has exactly that size. The non-null lock is checked before copying. The owned snapshot prevents later parsing from retaining an external allocation pointer.

### Alert 821

**Sanitizer allocator instrumentation.** `DS161085` at `third_party/miniz/miniz_common.h:76`.

The flagged C allocator is enabled only by the combined `SUPERZIP_MINIZ_FUZZ_ALLOCATOR` and `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` configuration. Its free/realloc family matches. This deliberately makes allocations visible to ASan; production Windows allocation uses its separate HeapAlloc/HeapFree/HeapReAlloc contract. Removing the sanitizer path would weaken testing.

### Alert 822

**Win32 rename request copy.** `DS121708` at `src/core/file_publish.cpp:487`.

`windows_api_path` rejects embedded NULs and normalized paths of 32767 or more wchar_t elements before request geometry is calculated. Consequently byte multiplication, request addition and DWORD conversions are bounded on Windows x64. The request allocates the header offset, the complete path and a terminator; only the path bytes are copied into its variable-length filename region. Publication pins identities and checks the API result.

### Alert 823

**Bounded virtual ZIP read fixture.** `DS121708` at `tests/cpp/test_zip_compat.cpp:113`.

The virtual reader first bounds the requested range against the virtual archive. The copied interval is the intersection of that range and the fixed EOCD record. Source offset, destination offset and length therefore fit both buffers. This fixture intentionally exercises a large declared ZIP layout without allocating or writing it.

### Alert 824

**Bounded allocation test oracle.** `DS161085` at `tests/cpp/test_zip_compat.cpp:75`.

`bounded_zip_alloc` checks multiplication overflow, enforces a 1 MiB per-allocation cap, records requests and returns null when the cap is exceeded. The matching callbacks use realloc/free. These are deliberate C callbacks for testing the production reader allocation boundary.

### Alert 825

**Fixed Windows file identity.** `DS121708` at `src/core/file_manifest.cpp:152`.

After `GetFileInformationByHandleEx` succeeds, the copy transfers the 16-byte `FILE_ID_128.Identifier` into the 16-byte `SourceFileIdentity::file_id` array. Both complete objects are live and disjoint. File identity, reparse-point and publication tests retain the surrounding contract.

### Alert 838

**Gzip input borrow.** `cpp/stack-address-escape` at `src/gzip/gzip_stream.cpp:340`.

`compress_bytes` consumes each bounded input chunk synchronously. Its `InputBorrow` destructor clears `next_in` and `avail_in` on normal and exceptional exits before the caller storage expires. Compressor state and output storage have their own owners. Stream readback, temporary-buffer and failure controls remain in `test_compression_streams.cpp`.

### Alert 839

**Bzip2 input borrow.** `cpp/stack-address-escape` at `src/bzip2/bzip2_stream.cpp:222`.

`compress_bytes` drains `avail_in` synchronously. The local `InputBorrow` destructor clears the pointer and count even if compression or sink writing throws. The non-const C ABI reads the input; it does not retain it after the borrow. The compression-stream tests cover temporary input and failure behavior.

### Alert 846

**Required overlapping move semantics.** `DS154189` at `third_party/lz4/lz4.c:359`.

This report highlights the fallback `LZ4_memmove` macro, not an unbounded operation. Its uses require memmove semantics for overlapping dictionary regions. The source retains capacity/length validation at the codec boundaries. Replacing it with memcpy would be incorrect; this decision admits only the exact primitive-choice diagnostic, not future bounds findings.

### Alert 847

**Standard copy primitive at the codec boundary.** `DS121708` at `third_party/lz4/lz4.c:351`.

This report highlights the standard C fallback of `LZ4_memcpy`, paired with the compiler-builtin implementation in the other branch. The macro does not itself determine a copy extent. The reviewed call sites use bounded codec regions, including the decoder extension checks retained above. This decision covers this primitive-choice diagnostic and does not certify every possible codec input or admit a separate bounds report.

### Alert 848

**Debug newline output.** `DS154189` at `third_party/lz4/lz4.c:286`.

The call prints the constant newline literal to stderr. It is compiled only under `LZ4_DEBUG >= 2`; there is no attacker-selected format string.

### Alert 849

**Debug format output.** `DS154189` at `third_party/lz4/lz4.c:285`.

The debug variadic call receives format literals from the reviewed DEBUGLOG invocations in this translation unit. Input-derived values are arguments, not format strings. The branch is compiled only under `LZ4_DEBUG >= 2`.

### Alert 850

**Debug file and line output.** `DS154189` at `third_party/lz4/lz4.c:284`.

The format is the constant file/line diagnostic and its arguments are `__FILE__` and `__LINE__`. It is compiled only under `LZ4_DEBUG >= 2`; the flagged function name does not establish a format-string vulnerability.

### Alert 851

**Matched LZ4 allocator family.** `DS161085` at `third_party/lz4/lz4.c:225`.

The macro maps the default C allocator to the corresponding free/calloc family. Its allocation calls use fixed `LZ4_stream_t` or stream-decoder sizes, check allocation failure and free their owners on the corresponding exits. Custom and disabled allocator modes preserve their upstream contracts. This is an allocator-choice report, not evidence of an unbounded allocation or mismatched release.

### Alert 852

**Serial benchmark opt-in flag.** `DS154189` at `tests/cpp/test_suzip_gpu_prefix.cpp:1016`.

The test reads `SUPERZIP_PATTERN_GPU_BENCHMARK` once to decide whether an optional benchmark is enabled. It does not modify the environment concurrently, cache the returned pointer across changes, use the value as a command or treat it as trusted input for a buffer size. Production compression does not use this opt-in.

### Alert 853

**Deliberately staged header.** `cpp/missing-header-guard` at `third_party/lz4/lz4.h:1`.

`lz4.h` separately guards the public declarations, optional static-linking declarations and common internals. Defining `LZ4_STATIC_LINKING_ONLY` after a first public inclusion must expose the optional declarations on a later inclusion. A whole-file guard would break that documented interface. Each declaration group already has its own guard; repeated inclusion is intentional. A standalone MSVC contract passed as both C and C++20 after public-first/static-later/repeated inclusion. Adding a whole-file pragma-once guard to an isolated copy made both compilations fail at the required static function declaration, independently demonstrating the regression.

### Alert 854

**Reachable unsigned error sentinel.** `cpp/constant-comparison` at `third_party/lz4/lz4.c:2094`.

The fast literal-length path compares the `size_t` result against `rvl_error`, which has the same unsigned type and the value SIZE_MAX. `read_variable_length` returns this sentinel on invalid input. This is a reachable error branch, not an always-false comparison against signed minus one. The malformed-length regression requires rejection before output is changed.

### Alert 855

**Reachable unsigned error sentinel.** `cpp/constant-comparison` at `third_party/lz4/lz4.c:2129`.

The fast match-length path uses the same typed unsigned error sentinel. Its comparison must reject a failed length read before the result is added to a match length; deleting it would weaken malformed-input validation. The report treats the cast sentinel as signed minus one.

### Alert 856

**Reachable unsigned error sentinel.** `cpp/constant-comparison` at `third_party/lz4/lz4.c:2266`.

The safe literal-length path checks the typed SIZE_MAX sentinel before length arithmetic or copying. `read_variable_length` can return that value for a truncated extension. The independent unterminated-length/canary test exercises this decoder boundary.

### Alert 857

**Reachable unsigned error sentinel.** `cpp/constant-comparison` at `third_party/lz4/lz4.c:2347`.

The safe match-length path rejects the typed SIZE_MAX sentinel before using the decoded length. The unsigned comparison is intentional and reachable; this exact diagnostic is an incorrect signed-range inference, not a reason to remove the check.

### Alert 1450

**Silesia x-ray integrity.** `DS173237` at `tools/run_archive_comparison.py:39`.

The `x-ray` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1451

**Silesia xml integrity.** `DS173237` at `tools/run_archive_comparison.py:38`.

The `xml` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1452

**Silesia samba integrity.** `DS173237` at `tools/run_archive_comparison.py:37`.

The `samba` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1453

**Silesia ooffice integrity.** `DS173237` at `tools/run_archive_comparison.py:36`.

The `ooffice` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1454

**Silesia nci integrity.** `DS173237` at `tools/run_archive_comparison.py:35`.

The `nci` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1455

**Silesia mozilla integrity.** `DS173237` at `tools/run_archive_comparison.py:34`.

The `mozilla` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1456

**Silesia dickens integrity.** `DS173237` at `tools/run_archive_comparison.py:33`.

The `dickens` entry in `EXPECTED` pairs a public corpus member length with its SHA-256 integrity value. The benchmark compares local input bytes with this value before use; it is never supplied as authentication material. This decision covers this individual constant, not other strings in the file.

### Alert 1742

**Public AMD distribution integrity.** `DS173237` at `tools/rocm-sdk-lock.json:5`.

The value is the SHA-256 field for the public AMD HTTPS distribution in the ROCm lock file. The bootstrap compares downloaded bytes against it. The record truthfully calls it a locally observed digest, not an independently published AMD checksum. It is integrity metadata and grants no authentication capability.

### Alert 1743

**Rejected insecure redirect fixture.** `DS137138` at `tools/test_bootstrap_rocm_sdk.py:140`.

The HTTP URL is passed directly to `NoRedirect.redirect_request` inside `assertRaisesRegex`. No network request is made. The fixture verifies that redirects are rejected before following an insecure destination; changing it to HTTPS would remove this negative case.

### Alert 1749

**Chunk-boundary SHA-256 vector.** `DS173237` at `tests/cpp/test_integrity.cpp:531`.

The expected digest is independently reproducible with Python hashlib for 1,048,577 ASCII `a` bytes. It tests hashing across a chunk boundary and is not a credential.

### Alert 1750

**SHA-256 abc vector.** `DS173237` at `tests/cpp/test_integrity.cpp:529`.

The value is the public SHA-256 digest of `abc`, asserted against the production hashing implementation. It is not a credential.

### Alert 1751

**Empty-input SHA-256 vector.** `DS173237` at `tests/cpp/test_integrity.cpp:527`.

The value is the public SHA-256 digest of empty input, asserted against the production hashing implementation. It is not a credential.

### Alert 8347

**Synchronous PPMd input borrow.** `cpp/stack-address-escape` at `third_party/lzma_sdk/C/7zDec.c:159`.

`SzDecodePpmd` stores the caller stream only in its private `CPpmdWithInput` owner. All range decoding and callbacks finish synchronously; `Ppmd7_Free` and `ISzAlloc_Free` release the decoder and bridge before returning. Neither pointer is returned or registered with a worker. The existing PPMd tests independently check callback ownership, both allocation failures, I/O errors, truncation and null empty output. The separate empty-output pointer-arithmetic defect was repaired in f6014cb and remains covered by its regression.
