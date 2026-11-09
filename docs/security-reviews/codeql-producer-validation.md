# Current CodeQL Producer Validation

This review applies individually to the CodeQL 2.27.4 reports below. The
maintainer's authorization, "Fix defects; allow exact proven false-positive
dispositions," permits these decisions. Earlier 2.27.3 decisions did not
automatically admit this producer. The reported locations, complete sources,
caller closures and relevant regression contracts were rechecked before renewal.

The complete CPU and HIP-host reports are retained from
[the successful analysis run](https://github.com/strmt7/FlitzZip/actions/runs/42669705590)
of commit `2cefab340a789a7fb061626c92165df54ab872df`. The CPU report reproduces
the eleven ordinary C/C++ cases below; HIP-host reproduces those and the nine
device-semantics cases. Their native SARIF SHA-256 digests are respectively
`293cd71deff414c647ad51fc61c3132dc38c5bfb4bb16aab199e7d919ae9f3e2` and
`34d16e49623b82e0a3e3c6eb7322b11f915d5beb12d75f2043f165dc05e7efeb`.
The ledger retains each observed category explicitly, including complete
coordinates, scanner identity and source/context fingerprints.

The required-HIP Release registry and its independent readback, corruption,
lifetime and recovery controls passed for the direct-registration source at
`8a5a46d49a8b3ddaa7e4176950735434e3c3178d`. Subsequent tooling changes preserve
the native source and build-input identities. The later successful Windows CI
and sanitizer workflows qualify their own unchanged native inputs; no new GPU
execution, benchmark, performance result or complete device coverage is claimed
by this document. Current source/context matching remains required.

## Individual decisions

### Alert 703

At `src/gpu/hip_codec.hip.cpp:15`, `__constant__` qualifies the device CRC
table. It is not a variable name. The table has five reads in device CRC paths.
The [device review](codeql-hip-device-semantics.md#alert-703) identifies the
independent readback and corruption consumers.

### Alert 8452

At `src/gpu/hip_codec.hip.cpp:446`, local `block_end` bounds the immediately
following fill, raw and periodic CRC loops. Its uses remain present; deleting
the bound would change the decoder. See the
[device review](codeql-hip-device-semantics.md#alert-8452).

### Alert 8453

At `src/gpu/hip_codec_static_prefix.hip.cpp:117`, local `word_count` is the
bound of the following scratch-word loop. Static-prefix packing and independent
readback remain direct consumers. See the
[device review](codeql-hip-device-semantics.md#alert-8453).

### Alert 8454

At `src/gpu/hip_codec_static_prefix.hip.cpp:116`, local `word_base` contributes
to the following `atomicOr` destination. The value is used to place each
thread's words. See the [device review](codeql-hip-device-semantics.md#alert-8454).

### Alert 8455

At `src/gpu/hip_sparse_pattern.hip.cpp:11`, `__global__` qualifies
`count_sparse_positions_kernel`. The entry point is bound to the kernel table
and launched by `hip_sparse_pattern.cpp`; it is not an unused static variable.
See the [device review](codeql-hip-device-semantics.md#alert-8455).

### Alert 8456

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:123`, local `word_count` bounds
the following scratch-word loop. Its read is explicit in the same kernel.
See the [device review](codeql-hip-device-semantics.md#alert-8456).

### Alert 8457

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:122`, local `word_base` is used
in the following `atomicOr` destination. Removing it changes packing addresses.
See the [device review](codeql-hip-device-semantics.md#alert-8457).

### Alert 8458

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:113`, local `code` supplies the
bits shifted and masked by the immediately following packing loop. It is read.
See the [device review](codeql-hip-device-semantics.md#alert-8458).

### Alert 8459

At `src/gpu/hip_codec_adaptive_prefix.hip.cpp:112`, local `width` bounds that
packing loop. The read determines the number of encoded symbol bits.
See the [device review](codeql-hip-device-semantics.md#alert-8459).

### Alert 632

At `third_party/lhasa/lib/lha_input_stream.c:85`, the stream borrows the
documented opaque callback context. Production sessions destroy reader and
stream before their owned context; test contexts outlive their stream owners.
The copied callback table has separate ownership. See the
[full caller review](lha-callback-lifetime.md#alert-632).

### Alert 838

At `src/gzip/gzip_stream.cpp:372`, the input is consumed synchronously.
`InputBorrow` clears `next_in` and `avail_in` on return and exception unwinding,
before the caller's storage expires. See the
[borrow review](current-source-findings.md#alert-838).

### Alert 839

At `src/bzip2/bzip2_stream.cpp:222`, the legacy non-const C interface borrows
input only during synchronous draining. `InputBorrow` clears its pointer and
count on every exit. See the [borrow review](current-source-findings.md#alert-839).

### Alert 8347

At `third_party/lzma_sdk/C/7zDec.c:159`, the decoder and callback bridge share
one heap owner. Synchronous range decoding completes before model and common
owner release. The caller keeps the borrowed stream alive. See the
[ownership review](ppmd-input-lifetime-context.md#alert-8347).

### Alert 853

`third_party/lz4/lz4.h` deliberately has separate public, static-linking and
implementation inclusion phases. The public guard and later static guard
remain present. A whole-file guard would break staged inclusion. See the
[interface review](current-source-findings.md#alert-853).

### Alert 854

At `third_party/lz4/lz4.c:2094`, the fast literal-length path compares two
`size_t` values. `read_variable_length` can return the typed maximum-value
sentinel on malformed input; the rejection is reachable. See the
[sentinel review](current-source-findings.md#alert-854).

### Alert 855

At `third_party/lz4/lz4.c:2129`, the fast match-length path rejects the same
typed unsigned sentinel before extending the decoded length. It does not
compare a nonnegative value with a negative signed integer. See the
[sentinel review](current-source-findings.md#alert-855).

### Alert 856

At `third_party/lz4/lz4.c:2266`, the safe literal-length path rejects the
reachable unsigned error sentinel before length arithmetic and copying.
See the [sentinel review](current-source-findings.md#alert-856).

### Alert 857

At `third_party/lz4/lz4.c:2347`, the safe match-length path rejects that
sentinel before decoded-length use. Removing it admits a real decoder error.
See the [sentinel review](current-source-findings.md#alert-857).

### Alert 8422

At `tests/cpp/test_tar_compat.cpp:789`, the helper has a direct call at line
899 from the registered test at line 887. Eight fixtures independently check
archive records, payloads and readback. The unchanged runner executes the case.
See the [reachability review](codeql-tar-reader-context.md#alert-8422).

### Alert 8431

At `tests/cpp/test_gdi_back_buffer.cpp:177`, the local unsigned loop variable
advances from zero through seven. Distinct colors and progressive allocation
counts check each iteration, followed by a ninth valid allocation and release.
No callback captures or changes the variable. See the
[induction review](gdi-rejection-loop.md#alert-8431).

## Acceptance limits

The device extraction and inference mechanisms behind the contradicted
diagnostics remain unproven. These conclusions rest on the actual source,
ownership and independent consumers, not on a general assumption about HIP or
any particular query. No source was removed, relocated or renamed to conceal a
report. No query, category, scanner or failure gate was suppressed. Raw reports
remain retained. Changed source, callers, locations, producer identity or bound
context expires the individual decision. All unmatched reports and the final
hosted audit remain blocking.
