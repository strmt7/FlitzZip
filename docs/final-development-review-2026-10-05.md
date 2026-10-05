# Development Review, 5 October 2026

This is an ongoing evidence record, not final acceptance or release approval.
The maintainer requested a final development round, comprehensive consistency
review, actual HIP tests and frontend Neutron qualification. Publication of a
new prerelease remains subject to separate maintainer approval.

## Native Build Repair

Commit `bab6b88abb59d51d75127863c48225993d2c1586` failed both hosted Windows
builds and the CodeQL C++ database build. The common failure was `LNK2001` for
`superzip::dictionary::improve_neutron_replacements`, followed by `LNK1120`.
The newly added malformed-layout test called a HIP-only implementation from
the CPU-only test executable. A passing HIP build had not exposed that link
dependency.

The repair moves the existing device-independent geometry validator into the
shared dictionary candidate header. Both configurations use the same validator;
the HIP regression additionally exercises the real dispatcher and proves that
malformed geometry is rejected before its checkpoint or device work. Descriptor
and replacement counts are now admitted explicitly before indexing. No GPU
algorithm, driver setting, archive representation or numeric compression policy
changes in this repair.

The actual CPU-only test executable linked and passed the admission regression.
The ordinary HIP build and all seven CTest suites passed, including actual
device consumers, independent readback, cancellation and controller contracts.
The native input identity is
`945d00e4056167d22137606793d0369d48c33723d3806be8f6bd58fb5708959c`.
The build receipt identity is
`9d5dbf57eef14621e0b37b3fc47614a56d69751bd45fffb27d890fce3b9c2b07`.
Local logs are retained under ignored `out/`; hosted acceptance remains pending
until the repaired revision completes its workflows.

## Exact Public-Example Approval

The maintainer approved only the four original public Crawl4AI proxy examples
in the [individual review](security-crawl4ai-public-proxy-review-2026-10-05.md).
They have a separate cohort so the original NLTK approval ledger and its
complete source hash remain unchanged. The redaction consumer supports bounded
in-memory TAR/Gzip member review without extraction. Complete archive/member
and match commitments are integrity metadata, not credentials.

All sixteen selected tooling commands passed. The redaction contracts passed
eighteen tests, including real historical Git/archive consumers, exact source
and reported locations, independent cohort commitments, verified-finding
rejection, duplicate names, links, oversized members and expanded-size refusal.
Changed-file preflight reported no unresolved finding; every raw scanner result
remains retained. Actual hosted full-history acceptance is still pending.

## Coverage And Open Work

The repository-wide refactoring inventory reported eight large-file observations.
That inventory is planning evidence, not a line-by-line correctness audit.
Changed functions passed the executable documentation and complexity gate.
Frontend smoke now starts a real Neutron job through production UI routing.
The worker reports its captured mode through the existing log and history.
Independent CPU verification and required-HIP extraction restored the complete
65,536-byte correctness fixture, with actual GPU kernel telemetry. This bounded
filesystem fixture is not a performance benchmark. Selection, persistence,
numeric efforts, CPU-policy normalization and format normalization also passed.
All eight pages were inspected at regular and compact sizes, together with the
Neutron dropdown. The original failed mode-observation test is retained locally;
history details alone had not supplied a log assertion.

The HIP build and all seven CTest suites passed for native input identity
`0ce207302a1a99692d644507cc8dd6a63126813c4f3a25a736be2eac5ec17d6f`,
with build receipt identity
`f10c5b704238a662da1e85b68547a83ef2cfef93a8aa4a4b3845b790c538ce99`.
The repaired selected verifier completed all eight commands. No encoding
algorithm or numeric effort changed in this frontend observability repair.

Current validation, stability and permission documents now link the completed
public-corpus records. The implementation plan distinguishes active status
from historical approvals and checkpoints. A read-only inventory checked 274
inline local file targets across all 116 tracked Markdown documents and found
no missing file. That check does not validate anchors, reference-style links or
every document's semantic claims. All 39 graph consumer tests passed, including
real publication regeneration and hostile XML controls. The license notice
generator and three inventory contracts passed; all eight native dependency
roots and five adapted development notices remain covered. These checks do not
settle unknown historical rights or prove the release rebuild obligation.

Three subsequent Canterbury repetitions used the same qualified native input
and receipt, 256 KiB blocks and the existing Hyperfine/RAM transport. All 33
required-HIP observations validated 8,432,352 source bytes and recorded 75,183
actual kernel launches. Each repetition produced 1,008,754 complete modeled
single-file archive bytes from 2,810,784 source bytes. The
[retained measurements](benchmarks/data/neutron-canterbury-repeated-2026-10-05.json)
include per-file source identities and actual GPU telemetry. The study is
complete; timing qualification remains false because host-isolation evidence
is separate. No new corpus workload, payload disk write or timing superiority
claim is introduced.

The [public-corpus benchmark](neutron-public-corpus-benchmark.md) retains the
991-file Govdocs1 study and seven-setting Canterbury sweep. Those measurements
qualify their recorded source revisions. The batching improvement preserved
archive sizes; it is not evidence of a compression-ratio advance. No claim of
best-in-class compression, universal compatibility or a resolved system freeze
is made. The [stability investigation](neutron-stability-investigation-2026-10-05.md)
remains open.

The dependency review retains the actual one-time latest-version experiments
and the incompatible Pydantic/core result. Dependabot's upstream NLTK advisory,
the open dependency pull request, external scanner configuration and final
release licensing obligations remain separate from passing local tests.
The repository-variable inventory confirms the OIDC broker URL is absent;
the live scanner reports `GREENBONE_SECRET_PROVIDER_URL repository variable is required`.
The maintainer has been asked for its existing endpoint without credentials.
The official NLTK release remains `v3.10.3` and the advisory still has no first
patched version. The source repair is retained; alert metadata is not fabricated
or dismissed to manufacture final acceptance.

The subsequent Neutron composition batch passed twenty selected local commands,
all seven native suites and the 612-case main suite, the registered format
matrix, interoperability, instrumented fuzzing, Windows ASan and packaging.
Version-nine compound frames are closed, bounded, self-contained and created
only through Neutron's required-HIP path. Both required-HIP decode stages and
CRC execute on the device. Ordinary numeric encoding policies remain separate.
Two initial fixture assertions were corrected: one input legitimately chose an
improved uncomposed result, while the grouped test now reconstructs every exact
first-stage byte before checking its unchanged oracle. Independent decoder,
malformed-kind, downgrade and cancellation coverage remain intact.

The [source-bound Canterbury composition record](benchmarks/data/neutron-canterbury-compound-2026-10-05.json)
reports 744,182 complete modeled independent-file archive bytes in each of
three repetitions, from the same 2,810,784 source bytes. The retained preceding
implementation produced 1,008,754 bytes. The size decrease is 26.23%; no file
regressed. All 33 readbacks used real HIP and no payload disk writes. Timing
qualification remains false. These observations qualify their recorded source;
they do not claim established-format superiority or resolution of the freeze.

Agent startup now delivers the existing Crawl4AI directive directly from
AGENTS.md, with current launcher provenance and executable missing-rule/tool
mutation controls. It does not install or requalify unchanged crawler packages.

The stronger secondary-stage refinement passed all twelve selected commands,
including all seven native suites, real Neutron frontend creation, independent
CPU verification, required-HIP extraction, the format matrix and instrumented
fuzzing. The frontend fixture selects a version-nine compound frame. All eight
pages were inspected at regular and compact sizes, together with the Neutron
selector. The source also selects optional CPU execution before device work for
known CPU-only blocks or explicitly absent HIP. Errors after HIP selection
propagate instead of silently retrying the codec on CPU. Two device metadata
copies now validate owned extents and use typed representation conversion.
The GUI smoke cleanup admits only non-reparse descendants of its owned root.
Initial scanner and PowerShell lint failures were repaired at those boundaries;
the subsequent checks passed without new dispositions or suppressions.

The native input identity is
`1aaca9df2ca1e4c3bb2435cf4492e4087a1f7e1e0a47a64bc2906865f4d91b9c`;
the canonical successful build receipt identity is
`a6226ef9a3a0e97bdc05941e84c8372544b9c32768f33d96a2ef51c4b662eeca`.
The [33-observation refinement record](benchmarks/data/neutron-canterbury-secondary-2026-10-05.json)
produced 741,174 complete archive bytes in every repetition, a further 3,008-byte
decrease. Only the spreadsheet improved; no file regressed. Kernel launches
increased to 148,350 and native compression time to about 21 seconds per pass.
This small gain has a substantial computation cost and does not establish a
general size advantage or qualified timing result. Earlier Govdocs1 and block
sweeps remain qualified only for their recorded source revisions.

The published composition revision has passing lint, component contracts,
Zstandard sanitizers and Scorecard workflows. Several other jobs were cancelled
before execution with GitHub's annotation, "The job was not acquired by Runner
of type hosted even after multiple attempts". Those jobs remain unvalidated;
their cancellation is not treated as a passing check or a demonstrated code
failure. Final exact-revision hosted acceptance remains required.
