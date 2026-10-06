# Neutron star mode public-corpus benchmark

The [independent-context Pythia14M pilot](../../benchmarks/data/neutron-pythia14m-contexts-pilot-2026-10-06.json)
compressed the complete original checkpoint to **24,162,233** modeled archive
bytes at 1 MiB, saving **1,935,715** bytes against the same-setting pruning
baseline. Required-HIP bytewise readback passed with zero payload writes.
The [subsequent three complete model repetitions](../../benchmarks/data/neutron-pythia14m-contexts-block-1024-2026-10-06.json)
reproduced that exact size in every run, a 7.42% saving against the same-setting
Neutron baseline. No paired timing improvement is claimed.
The [three-run Canterbury context study](../../benchmarks/data/neutron-canterbury-contexts-block-1024-2026-10-06.json)
retained all eleven file sizes and 723,429-byte totals. One trained checkpoint
does not establish broad model benefits; unchanged Canterbury sizes demonstrate
no additional size improvement on that corpus. Comparative timing and this
new representation's full Govdocs1 coverage remain unqualified.

The [complete 1 MiB Govdocs pruning study](../../benchmarks/data/neutron-govdocs1-thread0-pruning-block-1024-2026-10-06.json)
admitted and bytewise-validated all 991 original files in RAM, producing
331,334,741 complete modeled independent-file archive bytes from 593,182,383
source bytes. It recorded 10,146,702 actual HIP kernel launches and
4,210.85 seconds of native compression. The complete study finished within
its unchanged 7,200-second deadline and retained Hyperfine's diagnostics.
Benchmark payload writes were zero; timing remains unqualified.

Against the earlier complete 256 KiB batched record, 652 files became smaller,
311 were equal and 28 grew, saving 8,302,516 bytes overall. Against the 643
matched files of the incomplete byte-plane attempt, 81 became smaller,
497 were equal and 65 grew, saving 1,021,912 bytes overall. These comparisons
include different block geometry and preceding algorithm changes. They do
not isolate pruning, establish a speedup or justify changing the default block
setting. Individual increases remain visible in the record.

The [exact graph-pruning study](../../benchmarks/data/neutron-canterbury-pruning-2026-10-06.json)
completed three further byte-exact repetitions over the eleven original
Canterbury files. Every file retained its prior size: each pass produced
741,174 complete modeled independent-file archive bytes from 2,810,784 source
bytes. Actual kernel launches fell from 412,062 to 384,309 across the 33
observations. Native compression totals were 55.05–55.45 seconds per pass.
These observations qualify sizes and launch counts; they do not qualify a
paired timing improvement or the full Govdocs1 corpus. No new representation,
device workspace or changes to ordinary numeric efforts were introduced.

The [byte-plane study](../../benchmarks/data/neutron-canterbury-byte-plane-2026-10-05.json)
completed three byte-exact Canterbury repetitions at 256 KiB. Every pass still
produced 741,174 complete modeled independent-file archive bytes; all eleven
file sizes matched the preceding round. Native compression totals increased
to 58.89–59.30 seconds and the study recorded 412,062 actual kernel launches,
against 148,350 previously. The additional trials have no measured size benefit
on this corpus. They remain isolated to Neutron. Correctness on a controlled
numeric fixture does not establish effectiveness on trained model checkpoints
or arbitrary files. The completed 1 MiB Govdocs1 study is separate; older Govdocs1
results below qualify only their own source revisions.

The [stronger secondary-stage study](../../benchmarks/data/neutron-canterbury-secondary-2026-10-05.json)
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

The preceding [version-nine composition study](../../benchmarks/data/neutron-canterbury-compound-2026-10-05.json)
completed three identical-size Canterbury repetitions at 256 KiB, with real
HIP readback and zero payload disk writes. It reduced complete modeled
independent-file archive bytes from 1,008,754 to 744,182 (26.23%). The preceding
Govdocs1 and block-setting results below retain their own source qualifications;
they are not measurements of this new representation. Timing superiority and
comparisons with established formats remain unqualified.

## Pruning-Round Canterbury Block Settings

The [complete seven-setting record](../../benchmarks/data/neutron-canterbury-pruning-block-settings-2026-10-06.json)
retains 99 actual observations: the existing three 256 KiB repetitions and
one complete repetition at each larger setting. All eleven original files
passed required-HIP bytewise RAM readback at every setting with zero payload
writes, the same native source/receipt and no omitted member. The unequal
repetition counts are explicit; their timings are observational.

| Block setting, KiB | Complete independent-file archive bytes per repetition |
| ---: | ---: |
| 256 | 741,174 |
| 512 | 724,740 |
| 1024 | 723,429 |
| 2048 | 723,429 |
| 4096 | 723,429 |
| 8192 | 723,429 |
| 16384 | 723,429 |

The 1 MiB setting is 5,843 bytes, about 0.8%, smaller than the independent
729,272-byte ZIP reference on this corpus. It remains larger than bzip2 and
XZ, and it does not establish a broadly dramatic improvement or the best
setting for another workload. Production settings and ordinary efforts are
unchanged. The 256 KiB comparisons below retain their exact stated geometry.

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

The `Pythia14M` preset selects one actual trained EleutherAI checkpoint,
`model.safetensors`, at immutable artifact revision
`94f7c35d5e9f2e9bac8ca839329f505b4d007d5d`. Its complete 28,143,920 bytes and
SHA-256 are pinned from the official Hugging Face repository API. EleutherAI
licenses Pythia models and copyrightable artifacts under Apache-2.0; the
permission catalog admits local inert-byte compression and original numerical
measurements while retaining the repository's payload-redistribution hold.
This single small model extends workload coverage; it cannot establish
general trained-model effectiveness. Acquisition follows the publisher's
bounded HTTPS delivery inside reviewed `huggingface.co` and `hf.co` domains,
then authenticates the complete artifact before any GPU work. No model
runtime, tensor selection, pickle deserialization or training code is used.

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
py -3 -B -m tools.neutron_corpus_benchmark --corpus Pythia14M --runs 1 --file-timeout 3600 --suite-timeout 7200
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

The [three-pass Canterbury follow-up](../../benchmarks/data/neutron-canterbury-repeated-2026-10-05.json)
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
The complete compact [per-file size record](../../benchmarks/data/neutron-govdocs1-thread0-baseline-2026-10-05.json)
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

The [transport review](../development/neutron-corpus-transport-review-2026-10-05.md) records its
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

The complete [batched Govdocs record](../../benchmarks/data/neutron-govdocs1-thread0-batched-2026-10-05.json)
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
The [complete block-settings record](../../benchmarks/data/neutron-canterbury-block-settings-2026-10-05.json)
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

The [complete RAM-only reference](../../benchmarks/data/neutron-canterbury-standard-size-reference-2026-10-06.json)
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

At 256 KiB, Neutron is larger than every compressed reference on this corpus. This result
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

## Retained Incomplete Govdocs Attempt, 6 October

The [retained partial record](../../benchmarks/data/neutron-govdocs1-thread0-byte-plane-partial-2026-10-06.json)
contains 643 completed original files before the unchanged 7,200-second suite
deadline terminated the worker. They cover 439,277,560 source bytes and
222,323,480 modeled complete archive bytes, with bytewise required-HIP readback
and zero payload disk writes. Compared with those same files in the earlier
batched implementation, 424 sizes decreased, 219 remained equal and none grew,
saving 4,681,153 bytes. This compares several subsequent algorithm rounds; it
does not isolate the byte-plane transform's contribution.

The controller retained the completed protocols and released its owned children
and measurement lease. That revision's entire 991-file study remains unqualified: 348 files
did not produce completed observations, and Hyperfine produced no complete
timing result. Native compression alone totaled 6,764.901 seconds across the
completed observations, with 16,560,123 actual kernel launches. Increasing a
deadline would not remedy that cost. The subsequent exact graph-pruning round
completed Canterbury, the Pythia checkpoint and a separate full Govdocs1 study;
it does not retroactively qualify the unfinished Govdocs1 observations.

## Actual Trained Checkpoint, 6 October

The subsequent [three-run 1 MiB study](../../benchmarks/data/neutron-pythia14m-pruning-block-1024-2026-10-06.json)
preserved the exact original file and produced 26,097,948 complete archive
bytes in every repetition, 79,358 bytes smaller than the 256 KiB setting.
Each repetition recorded 91,034 actual kernel launches and native compression
between 24.2921 and 25.6028 seconds. The earlier single 256 KiB observation
recorded 276,788 launches and 102.263 seconds. The settings differ and no
paired timing qualification is claimed. The standard-container measurements
are reused after matching their identical original source and encoder settings;
their codec work is not repeated. ZIP is still 83,909 bytes smaller than this
Neutron result, and bzip2 and XZ remain smaller as well.

The [complete Pythia14M HIP observation](../../benchmarks/data/neutron-pythia14m-pruning-2026-10-06.json)
compressed the entire original 28,143,920-byte artifact into 26,177,306 complete
modeled archive bytes, a 6.99% reduction. The single run performed bytewise
required-HIP readback with zero payload disk writes, recording 276,788 actual
kernel launches and 102.263 seconds of native compression. Its complete raw
protocol, Hyperfine output and exact native input/receipt identities remain
bound to the record. One run qualifies that original file's size and
correctness; it does not establish timing superiority, stability of every
system or effectiveness on other models.

The [independent size reference](../../benchmarks/data/neutron-pythia14m-standard-size-reference-2026-10-06.json)
reused that qualified GPU observation and authenticated the complete original
artifact again for standard-library container encoding and bytewise RAM
readback. The inert safetensors JSON header identifies 76 tensors, all `F16`;
no tensor payload was deserialized, selected or altered. Every container used
the native RAM model's `memory-benchmark.bin` name, and counts include full
container framing. The producer and settings match the Canterbury size
reference above, with one GiB admitted before XZ preset 9.

| Complete single-file container | Archive bytes |
| --- | ---: |
| Neutron, 256 KiB blocks | 26,177,306 |
| ZIP, DEFLATE level 9 | 26,014,039 |
| TAR, gzip level 9 | 26,014,034 |
| TAR, bzip2 level 9 | 25,251,543 |
| TAR, XZ preset 9 with extreme search | 25,345,744 |
| Uncompressed TAR | 28,149,760 |

Neutron is larger than every compressed reference on this checkpoint as well.
That statement applies to the pruning observations in this section. The
subsequent independently framed context result above is 24,162,233 bytes,
smaller than all four unchanged compressed references for the exact same
original artifact. This is a measured checkpoint benefit; the broader size
objective remains unmet. A representation change must be tested on
these actual bytes and broader natural corpora before any effectiveness claim.
Attribution: EleutherAI, Pythia, Apache-2.0; all measurements are original,
and repository payload redistribution remains on hold.
