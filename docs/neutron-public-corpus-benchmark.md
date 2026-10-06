# Neutron star mode public-corpus benchmark

## Latest Retained Evidence

These are complete original-file, RAM-only studies with required-HIP bytewise
readback. Each JSON retains its measured source and binary identities. Reusing
an observation does not turn it into a measurement of a later revision.

| Corpus and retained revision | Original files | Input bytes | Complete modeled archive bytes | Evidence |
| --- | ---: | ---: | ---: | --- |
| Canterbury, version-eleven contexts with admitted kernel module, 1 MiB blocks | 11 | 2,810,784 | 723,429 in each of three runs | [Study](benchmarks/data/neutron-canterbury-lazy-kernel-block-1024-2026-10-06.json) |
| Pythia14M, version-eleven contexts with admitted kernel module, 1 MiB blocks | 1 complete checkpoint | 28,143,920 | 24,162,233 in each of three runs | [Study](benchmarks/data/neutron-pythia14m-lazy-kernel-block-1024-2026-10-06.json) |
| Govdocs1 thread0, preceding pruning revision, 1 MiB blocks | 991 | 593,182,383 | 331,334,741 | [Study](benchmarks/data/neutron-govdocs1-thread0-pruning-block-1024-2026-10-06.json) |

The fresh module-boundary studies preserve every original file's preceding
context-study size in three repetitions, with actual HIP encoding and decoding.
The [preceding Canterbury](benchmarks/data/neutron-canterbury-contexts-block-1024-2026-10-06.json)
and [Pythia studies](benchmarks/data/neutron-pythia14m-contexts-block-1024-2026-10-06.json)
remain unchanged. The context refinement saved 1,935,715 bytes (7.42%) on the complete Pythia
checkpoint against its same-setting Neutron baseline. Canterbury retained the
preceding 1 MiB sizes. The complete Govdocs1 study belongs to its preceding
revision; full version-eleven context coverage remains unqualified.
Every record has `memory_only=true`, `disk_write_bytes=0` and
`timing_qualified=false`. Sizes and correctness are qualified within the recorded
scope; comparative speed, universal GPU stability and universal compression
superiority are not established.

## Standard-Format Size Reference

The [complete reference](benchmarks/data/neutron-canterbury-standard-size-reference-2026-10-06.json)
uses the same eleven unmodified Canterbury files and counts full independent-file
containers, including headers, directories and padding. Each member was restored
bytewise in RAM. Different algorithms have different windows and search budgets;
numeric effort labels do not imply equal work.

| Complete independent-file containers | Total archive bytes |
| --- | ---: |
| Neutron contexts, 1 MiB blocks | 723,429 |
| Preceding Neutron, 256 KiB blocks | 741,174 |
| ZIP, DEFLATE level 9 | 729,272 |
| TAR, gzip level 9 | 729,330 |
| TAR, bzip2 level 9 | 543,236 |
| TAR, XZ preset 9 with extreme search | 494,376 |
| Uncompressed TAR | 2,887,680 |

The retained 1 MiB Neutron result is slightly smaller than these ZIP and gzip
containers but substantially larger than bzip2 and XZ. The target of dramatically
smaller archives remains unmet on this corpus. This is a size comparison,
not a controlled application-speed or solid whole-corpus archive comparison.

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

Subsequent candidates must compare every original file and retain the prior
winner. Include all metadata costs, byte-exact readback, actual HIP telemetry
and complete workload coverage. Size gains on one model or file type cannot
establish broad effectiveness. Numeric efforts 1–9 remain separate from Neutron.

Earlier rounds, unsuccessful complete-corpus attempts and their original claims
remain in [the benchmark history](history/benchmarks/neutron-public-corpus-rounds.md).
The reported system-freeze cause remains unconfirmed; successful studies do not
establish its resolution.
