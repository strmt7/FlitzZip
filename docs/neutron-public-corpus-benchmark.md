# Neutron star mode public-corpus benchmark

The [byte-plane study](benchmarks/data/neutron-canterbury-byte-plane-2026-10-05.json)
completed three byte-exact Canterbury repetitions at 256 KiB. Every pass still
produced 741,174 complete modeled independent-file archive bytes; all eleven
file sizes matched the preceding round. Native compression totals increased
to 58.89–59.30 seconds and the study recorded 412,062 actual kernel launches,
against 148,350 previously. The additional trials have no measured size benefit
on this corpus. They remain isolated to Neutron. Correctness on a controlled
numeric fixture does not establish effectiveness on trained model checkpoints
or arbitrary files. The current Govdocs1 study is separate; older Govdocs1
results below qualify only their own source revisions.

The [stronger secondary-stage study](benchmarks/data/neutron-canterbury-secondary-2026-10-05.json)
completed three byte-exact Canterbury repetitions at 256 KiB, producing 741,174
complete modeled independent-file archive bytes from 2,810,784 source bytes.
It saved 3,008 bytes (0.40%) against the preceding composition round. Only
`kennedy.xls` became smaller; the other ten files retained identical sizes.
All 33 observations used real HIP and zero payload disk writes. This is a
small, workload-dependent gain, not evidence of broadly dramatic compression.
The native compression totals were 20.91–21.09 seconds per repetition, against
about 8.7 seconds previously; actual kernel launches increased from 75,567 to
148,350 across the study. Timing qualification remains false. The additional
effort therefore has a measured size benefit and a substantial observed cost.

The preceding [version-nine composition study](benchmarks/data/neutron-canterbury-compound-2026-10-05.json)
completed three identical-size Canterbury repetitions at 256 KiB, with real
HIP readback and zero payload disk writes. It reduced complete modeled
independent-file archive bytes from 1,008,754 to 744,182 (26.23%). The preceding
Govdocs1 and block-setting results below retain their own source qualifications;
they are not measurements of this new representation. Timing superiority and
comparisons with established formats remain unqualified.

## Workloads

The [Canterbury corpus](https://corpus.canterbury.ac.nz/descriptions/) is a fixed
lossless-compression benchmark of eleven natural files, totaling 2,810,784 bytes.
Use the original university TAR archive and its pinned complete inventory.
Some mirrors normalize text line endings: equal names do not establish equal
benchmark inputs. Arnold and Bell, *A corpus for the evaluation of lossless
compression algorithms*, DCC 1997, describe its selection. Its small size and age
limit production representativeness.

[Govdocs1](https://digitalcorpora.org/corpora/file-corpora/files/) supplies
standardized pilot-study subsets of real documents. The current published
thread0 object contains 991 files, 593,182,383 decoded bytes and a largest file
of 175,101,388 bytes. Run every current member, including that largest file;
do not fabricate the historical nominal count of 1,000. This broad forensic
corpus complements Canterbury and does not establish universal compression
rankings or represent current model checkpoints. Cite Garfinkel, Farrell,
Roussev and Dinolt, *Bringing Science to Digital Forensics with Standardized
Forensic Corpora*, DFRWS 2009.

The publisher warns that some Govdocs1 members contain malware. The controller
handles inert byte sequences exclusively. It never executes or renders members,
extracts them to a filesystem, or changes antivirus settings. Benchmark usage
and original measurements are reviewed in `tools/benchmark_permissions.json`;
payload redistribution remains on hold. The [publisher terms](https://digitalcorpora.org/about-digitalcorpora/terms-of-use/)
retain separately claimed constituent copyrights.

## Existing Tools And RAM Transport

Use the installed reviewed Hyperfine 1.20.0 for repetitions and overall command
timing. Its command adapter feeds the existing production `memory-benchmark`
engine; it does not invent another compressor or timing/statistics engine.
Successful timing-tool diagnostics are retained as text, original Base64 bytes
and SHA-256. Native telemetry still rejects stderr, and all nonzero exits and
bounded-output violations fail. Size qualification requires every expected
observation and unchanged native qualification. A successful tool exit or an
outlier warning cannot establish host isolation; timing remains unqualified.

```powershell
py -3 -B -m tools.neutron_corpus_benchmark --corpus Canterbury --runs 3
py -3 -B -m tools.neutron_corpus_benchmark --corpus Govdocs1Thread0 --runs 1 --file-timeout 3600 --suite-timeout 7200
```

Acquire each published archive once per study, decode with bounded standard
library readers, then retain original files in RAM for every repetition. A
Windows shared-memory exchange carries those bytes to the adapter, using fresh
unpredictable names and the creating token's default object access policy.
No network listener or HTTP request parser exists. Workers receive read-only
source views, check bounded contiguous member extents and authenticate each
input. A Windows mutex serializes complete repetitions; concurrent or abandoned
workers invalidate the study. Fixed-size RAM slots retain native observations,
with gaps, duplicates, excess output and partial runs rejected. Binary stdin carries each
file into the native engine. No corpus, encoded archive, decoded payload or
benchmark journal is written to disk. Python's `-B` disables bytecode-cache
writes. Ordinary executable/module loading and unrelated operating-system I/O
are outside the codec's `disk_write_bytes=0` claim.

The revised CLI source accepts a declared 32-bit source extent and applies physical-memory
admission before loading it; the old fixed 64 MiB corpus ceiling is removed.
The study separately admits simultaneous compressed/decoded corpus residency
and child input copies under half of available RAM with at least two GiB free.
Each member is authenticated by SHA-256 before timing and compared byte-for-byte
after HIP decode. Native chunk, queue and GPU workspace limits remain active.

Validate the current successful HIP receipt when acquiring a whole-study lease
and at study completion. Hold deny-write/delete handles for measured native
outputs, build transaction and corpus/tool definitions. Do not rehash the entire
repository before and after every file. A failed, changed or incomplete study
does not qualify results. Per-process and study deadlines clean up only the
controller's owned process tree; acquisition has a separate finite deadline.
The default per-file deadline remains 300 seconds. A caller may explicitly
select up to 3600 seconds for a slow natural file, while the separate complete
study deadline remains bounded to at most 7200 seconds. This changes process
lifetime admission, not HIP launch size, synchronization, cancellation, memory
admission or compression search. A timeout is a failed study, never permission
to omit, truncate or replace the file.

The first complete Govdocs attempt on 5 October 2026 reached 175 byte-validated
files, covering 82,765,977 input bytes and 65,430,295 complete single-file archive
bytes. The following 175,101,388-byte natural file exceeded the explicitly
selected 600-second deadline. The controller terminated its owned tree and
retained the completed prefix with `study_qualified=false`; these partial totals
are not a Govdocs baseline. All 991 original members were admitted from the
published ZIP, whose measured SHA-256 was
`662343a0725b350d1d37496b8d2a53ee85bfa27a48e05209ddd8d3de5163e84a`.
This timeout establishes an incomplete workload, not the cause of the earlier
system freeze or a GPU speedup.

## Reporting And Limits

The [three-pass Canterbury follow-up](benchmarks/data/neutron-canterbury-repeated-2026-10-05.json)
records 33 complete required-HIP observations at 256 KiB on the frontend-tested
revision. Every repetition retained the preceding 1,008,754 complete modeled
single-file archive bytes and restored every original source byte. The retained
summary qualifies the complete study; streamed per-file records remain
unqualified until that final source-bound summary succeeds. The follow-up is
a current baseline for Neutron size experiments, not a new algorithm or a
timing superiority claim.

After Hyperfine's owned workers finish, emit completed per-file native protocols
from the retained RAM slots to stdout; collect the whole stream without truncating
tool output. A completed study
reports corpus/member hashes, exact input/payload/complete single-file archive
bytes, Neutron mode, settings, actual HIP telemetry, bytewise validation and
Hyperfine's summary. Aggregate complete archive sizes count one independent
archive per natural file. They are not a measurement of one multi-entry archive.

Failed studies emit their completed observation prefix with
`study_qualified=false`; partial totals cannot qualify a complete corpus run.

Hyperfine measures adapter, transport and process-start overhead as well as
native work. The native protocol reports its own compression, verification,
extraction and validation phases. Shared-host resource-isolation qualification
is still required for speed claims; the controller explicitly leaves
`timing_qualified=false`. Deterministic sizes and correctness remain useful
without interpreting noisy timing as improvement.

One Govdocs pass establishes coverage and exact sizes. Subsequent repeated
comparisons must preserve the complete workload and preceding-source identities;
do not extrapolate one pass into statistical confidence. Corpora are distinct
workloads: report them separately. No Neutron result belongs in the ordinary
levels 1–9 benchmark series.

The shared-memory API's [Windows lifetime contract](https://docs.python.org/3/library/multiprocessing.shared_memory.html)
deletes mappings after all handles close. The standard library manages that
lifetime; the controller creates no backing payload files. Windows documents
the [default object access policy](https://learn.microsoft.com/en-us/windows/win32/memory/file-mapping-security-and-access-rights)
and [abandoned-mutex semantics](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject).
The exchange is local process coordination, not a boundary against another
process already authorized by the same account's object policy.

The subsequent complete Govdocs run admitted and bytewise-validated all **991**
original files: **593,182,383 input bytes** produced **339,637,257 modeled complete
single-file archive bytes**, or **57.2568%** of the input total. The original
175,101,388-byte file completed under the explicit 3,600-second per-file budget;
the 7,200-second suite ceiling and all GPU launch/workspace limits were retained.
The complete compact [per-file size record](benchmarks/data/neutron-govdocs1-thread0-baseline-2026-10-05.json)
preserves every natural file, the exact native build identities and the digest
of the original observation stream. The source ZIP is the same measured archive
as the earlier failed prefix. No corpus payload is redistributed.

The protocol reports `memory_only=true`, `disk_write_bytes=0`, 593,182,383 validated
bytes and 5,613,659 actual GPU kernel launches. Native compression totaled
2,104.565 seconds; Hyperfine's full worker run was 2,563.039 seconds. This single
measured pass qualifies workload coverage, deterministic sizes and readback,
with `timing_qualified=false`; it establishes neither GPU superiority nor a
controlled throughput comparison. Corpus, archive and restored payloads remained
in RAM. This is not a claim of zero operating-system or reporting I/O.

Offline fixtures and the completed corpus do not establish universal GPU
stability or explain the earlier system freeze. Its cause remains unconfirmed.

The retained natural-file results also identify where this workload's bytes
remain. These groups use published filename extensions; they do not establish
payload types or substitute a smaller subset for the complete corpus.

| Extension | Files | Original bytes | Complete archive bytes | Archive/input |
| --- | ---: | ---: | ---: | ---: |
| txt | 84 | 184,623,823 | 24,110,688 | 13.06% |
| pdf | 257 | 152,833,695 | 125,940,522 | 82.40% |
| ppt | 54 | 118,157,466 | 104,955,562 | 88.83% |
| jpg | 104 | 40,228,145 | 39,532,198 | 98.27% |
| xls | 60 | 28,383,868 | 12,245,671 | 43.14% |
| doc | 67 | 27,133,440 | 15,729,225 | 57.97% |

Subsequent Neutron candidates must compare complete encoded sizes on every
original file and retain the prior winner. Favoring one group or changing a
representation is not an improvement until byte-exact readback, total metadata
cost and the complete corpus comparison have passed.

The [transport review](neutron-corpus-transport-review-2026-10-05.md) records its
ownership boundaries, real Hyperfine integration and explicitly offline evidence.

## Neutron Batching Round

Neutron now shares its existing bounded parse across adjacent, complete,
64 KiB-aligned archive blocks, up to the unchanged one-MiB primitive limit.
Each block retains its independent framing and preceding winner. The minimum-byte
parse, per-launch work, kernels, driver boundary and ordinary numeric levels
are unchanged. The complete descriptor layout is admitted before device-pointer
arithmetic, GPU work or replacement mutation.

The HIP Release build and all 32 affected native cases passed. The added
regressions reject malformed complete layouts before the device checkpoint,
compare all sixteen batched segment payloads byte-for-byte against isolated
calls with independent CPU/HIP readback, and exercise preservation of losing
trials through the real production dispatcher. Existing cancellation,
workspace, archive readback and ordinary-level consumer checks remain covered.

The complete [batched Govdocs record](benchmarks/data/neutron-govdocs1-thread0-batched-2026-10-05.json)
compares every original file, in order, against the retained baseline. All
**991 archive sizes are identical**, with no regressions: 593,182,383 input bytes
still produce 339,637,257 complete independent single-file archive bytes.
Required-HIP bytewise readback covers the complete input. Corpus, archive and
restored payloads remained in RAM with `disk_write_bytes=0`.

Actual kernel launches fell from 5,613,659 to **2,776,684**, removing 2,836,975
launches, approximately 50.5%. Native compression totaled **934.165 seconds**,
55.6% less than the preceding 2,104.565-second measurement. Hyperfine's complete
worker run took **1,401.004 seconds**, 45.3% less than the preceding 2,563.039
seconds. These are observed elapsed-time improvements for the same workload,
block size and benchmark tool, with identical per-file sizes and complete
readback.

The maintainer reports that the PC was near-idle. No recorded system-load
telemetry establishes contention. The `timing_qualified=false` label records
that paired repetitions and system-load telemetry were not collected; it does
not mean that background load was detected or that the measured times are
invalid. A single before/after comparison does not establish statistical
variation or a universal speedup. The earlier system-freeze cause remains
unconfirmed.

The accepted run binds native inputs
`f74dca25f3befecb5f73d2a23bb6ebd5c405478767c7d1caf6c2322c6ba23712`
and build receipt
`ae32f072cc4316a9e37a57e1a08a2b2e6d395bbcb98b8877507d4a846be78326`.
The original publisher ZIP hash, all per-file sizes and the full observation
stream digest are retained in the new record. Neither corpus nor archive
payloads are redistributed.

The same native build then completed all eleven original Canterbury files at
each supported block-size setting, totaling 77 byte-exact required-HIP readbacks.
The [complete block-settings record](benchmarks/data/neutron-canterbury-block-settings-2026-10-05.json)
retains the original publisher order, every file size, real launch counts and
each complete observation-stream digest. Every run reports RAM-only payloads
and zero benchmark payload disk writes. The 256 KiB result exactly matches the
preceding 1,008,754-byte Canterbury total; totals at different block sizes are
separate settings, rather than comparisons against that baseline.

| Block size (KiB) | Original input bytes | Complete single-file archive bytes |
| --- | --- | --- |
| 256 | 2,810,784 | 1,008,754 |
| 512 | 2,810,784 | 1,008,644 |
| 1,024 | 2,810,784 | 1,008,622 |
| 2,048 | 2,810,784 | 1,008,622 |
| 4,096 | 2,810,784 | 1,008,622 |
| 8,192 | 2,810,784 | 1,008,622 |
| 16,384 | 2,810,784 | 1,008,622 |

These single-pass setting checks establish coverage and recovery. They do not
change the default block size or qualify a throughput comparison.

## Standard-Format Size Reference, 6 October

The [complete RAM-only reference](benchmarks/data/neutron-canterbury-standard-size-reference-2026-10-06.json)
uses all eleven original Canterbury files and the exact source digests from
the current 33-observation Neutron study. Each independent container uses the
native RAM model's `memory-benchmark.bin` entry name. Complete container bytes
are counted, including the ZIP directory or TAR headers and padding. Every
standard-codec decompression and archive-member read was compared bytewise
with its entire original source. No corpus or archive payload was written to
disk, and the GPU observations were reused rather than repeated.

| Complete independent-file containers | Total archive bytes |
| --- | ---: |
| Current Neutron, 256 KiB blocks | 741,174 |
| ZIP, DEFLATE level 9 | 729,272 |
| TAR, gzip level 9 | 729,330 |
| TAR, bzip2 level 9 | 543,236 |
| TAR, XZ preset 9 with extreme search | 494,376 |
| Uncompressed TAR | 2,887,680 |

Neutron is larger than every compressed reference on this corpus. This result
does not satisfy the target of dramatically smaller archives. Uncompressed
TAR is an inventory/container baseline, not a competitive compression method.
The measurements are size references, not application-speed comparisons or a
solid whole-corpus archive study. Settings, windows and dictionaries differ
between algorithms; their numeric effort labels do not imply equal work.

The producer is CPython 3.13.16 with zlib 1.3.1, using the existing standard
`zipfile`, `tarfile`, `gzip`, `bz2` and `lzma` implementations. ZIP uses a fixed
1980 timestamp, DEFLATE 9 and regular-file mode 0644. TAR uses USTAR, mode 0644,
zero timestamps/IDs and empty owner names; each complete TAR is then filtered
using `gzip.compress(..., compresslevel=9, mtime=0)`, `bz2.compress(...,
compresslevel=9)` or `lzma.compress(..., format=FORMAT_XZ, preset=9 |
PRESET_EXTREME)`. Source acquisition and original-member validation use the
existing canonical corpus controller. The owned producer ran below normal
priority and admitted one GiB before compression because
[preset 9 can require up to 800 MiB](https://docs.python.org/3.13/library/lzma.html).
The local producer and full successful output remain under ignored `out/`.
Only measurement metadata is published; no comparator runtime is added to the
application and no external project code is copied.
