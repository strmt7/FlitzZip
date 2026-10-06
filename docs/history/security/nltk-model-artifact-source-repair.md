# NLTK Model-Artifact Source Repair

The Crawl4AI development dependency NLTK 3.10.3 is affected by
[GHSA-8mgp-746c-j5xp / CVE-2026-81726](https://github.com/nltk/nltk/security/advisories/GHSA-8mgp-746c-j5xp).
The six reported APIs accept caller-controlled model paths and bypass the
enforcing `nltk.pathsec` boundary. This was reproduced locally against the
actual installed release; it is a source vulnerability, not harmless prose or
an accepted risk.

No patched stable NLTK release was available at the review checkpoint. The
repository builds immutable upstream development source at
[1bd574cfd4e3bca82c6b145d9271a646df410707](https://github.com/nltk/nltk/commit/1bd574cfd4e3bca82c6b145d9271a646df410707),
which includes the merged path validation, guarded I/O, private model staging
and narrow deserialization repairs. The installed distribution and runtime
identify themselves as `3.10.3+superzip.security1.g1bd574cfd`. This identifies a
different source build honestly; it does not claim that upstream published a
patched release. No API is renamed to evade a rule, no installed source is
monkeypatched and no source alert is dismissed by this repair.

## Artifact And Installation Admission

The [provenance directory](../../../third_party/upstream/nltk/README.md) retains the
unmodified official source archive and original notices. The
[builder](../../../tools/nltk_security_build.py) verifies its digest, bounds extraction,
rejects unsafe members, uses the separate complete build-tool lock and produces
a standard pure-Python wheel. Runtime source bytes remain identical to the
selected upstream commit. Packaging changes identify the downstream build and
retain a prominent change notice alongside the original licenses.

Canonical packaging preserves all runtime and original license payloads,
normalizes generated metadata line endings, updates standard wheel `RECORD`
hashes and fixes ZIP metadata. Stored ZIP members make the admitted wheel
independent of host zlib versions. A wrong source or wheel digest fails closed.
No generated wheel is committed.

The crawler installs its entire locked graph normally with `--require-hashes`
and binary-only runtime selection, including the admitted local wheel. It
requires `pip check` and behavioral API/consumer regressions before publishing
the installation receipt. Build-only `--no-deps` cannot reach runtime
installation. The installer owns its processes and external caches; it does
not alter the archive application, GPU driver or host access controls.

## Retained Verification

The released source failed all six outside-root refusal tests, without fixture
errors. The repaired installed wheel passed those six tests, legitimate
perceptron/tagger roundtrips and Crawl4AI's actual NLTK stemming/BM25 consumer.
Each refusal checks an enforcing root and an existing or writable outside
target; the tests cannot pass by never reaching a usable operation. Parser
training uses a small backend fixture to reach serialization, without training
a model or installing optional machine-learning extras.

The broader upstream security run passed 1,408 tests and skipped 325 cases.
Three resource-dependent failures were retained and diagnosed: two required
Punkt parameters and one required the English tagger. Those same tests passed
in targeted reruns with tiny valid local resource fixtures. No pretrained
models were downloaded. POSIX-only and optional-resource skips remain skips;
this is not a claim of complete NLTK functional or cross-platform validation.

The repaired crawler then qualified 30 of 32 public websites on Windows.
The GNU timeout and W3C challenge remained failures. The receipt binds the
integration, locks, source archive and build admission; no unchanged website
matrix or native build is repeatedly run.

An OSV query of the complete locked installed-version graph returned no
advisories at the checkpoint. A separate query of the original upstream
3.10.3 version still returned this advisory. Version-database silence for a
downstream version is not proof of source repair: the original-code negative
control, exact upstream bytes and repaired API behavior provide that evidence.
Retain the raw advisory and reports. New advisories need fresh source review;
this document does not authorize a general NLTK or dependency suppression.

Ignored local evidence includes `out/nltk-original-model-regressions-final.json`,
`out/nltk-final-admission-evidence.json`, the upstream JUnit reports,
`out/crawl4ai-osv-candidate-query.json`, the separate upstream-version query and
`out/nltk-crawler-qualification-final-20261005.json`. The actual pushed commit
`ef3ff5c223e7033355036148467ce36d1475f693` passes all four hosted platform
jobs, including normal dependency resolution, `pip check` and the model/API
regressions. Its security workflow passes the dependency and source scanners
but remains failed on the separately reviewed upstream SSRF test URI in the
secret-history scan. The high-severity Dependabot alert also remains open;
the advisory has no patched upstream release. Neither result is claimed closed.

## Continuing Maintenance

Keep the source/model tests connected to production installation and the
verifier. Dependency locks with nonstandard names need explicit scanner
discovery; an empty report does not prove they were read. Both locks have
explicit OSV inputs and their directory is covered by Dependabot.

Prefer a compatible patched upstream release when it becomes available. Retire
the downstream source build only after normal locked resolution, model API
regressions, consumer checks and the relevant platform workflows pass. Keep
unresolved reports and platform gaps visible; do not change vulnerability gates
to obtain a green checkpoint.

## Installed Source Integrity, 6 October

Cached crawler startup previously compared NLTK's distribution version without
checking its installed Python source. It now compares a deterministic inventory
and content digest with the admitted upstream archive, including the correctly
identified downstream `VERSION` file, before importing package code. Every
Python source member participates; changed, missing or additional source files
are refused even when installed distribution metadata still claims the same
version. Runtime source inspection rejects redirects and special files and
uses bounded directory traversal and reads. It does not execute inspected code.

Source extraction and the integrity reader share complete ZIP metadata
admission. Ambiguous case aliases, normalized-path aliases, special members
and duplicate or escaping entries are rejected before extraction writes.
The archive, wheel, version and locked dependency graph are unchanged; this
guard does not rebuild a valid admitted wheel or reinstall a qualified graph.
The original six-API negative control and repaired nine-test model/consumer
evidence still bind the same runtime source. New source-integrity contracts and
an actual installed-cache consumer are required for this guard; their results
are recorded in the current development review.

## Executed Source Integrity

A separate inert-package control reproduced a gap between source admission and
execution: the source digest passed after the original initializer was restored,
but ordinary Python imported different unchecked-hash bytecode from its cache.
The isolated source-import control loaded the original initializer instead.
This test changed only an owned temporary fixture, never the installed package.

Crawler runtime commands now use a fresh temporary
[`sys.pycache_prefix`](https://docs.python.org/3/library/sys.html#sys.pycache_prefix)
and `-B` through the standard interpreter. Python documents that this prefix
changes where caches are read and ignores source-tree `__pycache__` directories;
`-B` prevents writing new bytecode. The prefix is scoped to the contained child
and removed after its process tree exits. Existing source and cache files are
preserved. The pure-Python NLTK admission also rejects native executable modules
and sourceless bytecode outside ordinary cache directories.

The source-import contracts exercise an actual unchecked-hash cache, exact CLI
argument forwarding, child-only flags, temporary-directory lifetime and refusal
of native or sourceless module additions. The unchanged admitted installed
runtime passed all nine model/API tests under this policy. Crawl4AI then fetched
the primary Python documentation with robots checking enabled. No package or
browser reinstall was required. The earlier multi-site qualification remains
evidence for its original checkpoint; this focused check qualifies the changed
import mechanism and its actual consumers.
