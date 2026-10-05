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
Frontend smoke currently proves Neutron selection and persistence; an actual
GUI-created Neutron archive and independent CLI readback are the next consumer
checks. Current documents that still describe the completed public-corpus study
as pending must be reconciled with its source-bound retained results.

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
