# Development Dependency Remediation

The earlier temporary acceptance of
[NLTK advisory GHSA-8mgp-746c-j5xp](https://github.com/advisories/GHSA-8mgp-746c-j5xp)
applied to the isolated development crawler's previous dependency pin. The
crawler now uses the unmodified official wheel from
[stable v3.10.4](https://github.com/nltk/nltk/releases/tag/v3.10.4), whose package
metadata reports version `3.10.4`. The maintainer's earlier candidate exception
remains specific to that superseded artifact.

The [upstream distribution record](../../third_party/upstream/nltk/3.10.4/distribution.json)
binds exact asset bytes and original notices. The installer retrieves the wheel
into its external cache through normal pip HTTPS transport, verifies it before
atomic publication, and refuses corrupted cache entries. Built wheels are not
tracked in Git. Installed-source admission,
model-artifact failure and positive-flow regressions, selected upstream
path-security suites and real crawler consumers qualify the candidate.
Platform and optional-feature skips remain explicit. The locally retained
upstream wheel/source comparison and test receipts are evidence for that exact
artifact, not a claim that every optional NLTK feature has been tested. The normal
hash-locked requirements select its exact release URL. A package index cannot
substitute another same-version wheel; every transitive dependency still resolves
normally under its existing version and hash constraints.

Aiohttp is pinned to `3.14.4`, the patched version for
[GHSA-5wr8-gcqf-gqf3](https://github.com/advisories/GHSA-5wr8-gcqf-gqf3),
[GHSA-494j-54xm-rwp3](https://github.com/advisories/GHSA-494j-54xm-rwp3) and
[GHSA-2g87-pp6c-29x5](https://github.com/advisories/GHSA-2g87-pp6c-29x5).
The complete crawler graph resolves normally. The upstream security regressions
exercise initial-connection deadlines, Secure cookies and bounded parser,
WebSocket, cookie and decompression behavior.

The earlier candidate NLTK/aiohttp graph passed clean installation, installed API contracts, browser
extraction and cache reuse on Windows, macOS and Linux. Website qualification
retains both successful extraction and access failures; a timeout or a site
challenge is not reported as a successful crawl. The exact pushed dependency
audit reports the preceding alerts fixed, without manual dismissal.

Pillow is pinned to `13.0.0`, the patched release for the decoder advisories
including [GHSA-8g5g-8vmr-9782](https://github.com/advisories/GHSA-8g5g-8vmr-9782).
Its JPEG decoder rejects output dimensions that exceed the declared tile before
writing image data. The update satisfies Crawl4AI's published `pillow>=10.4`
constraint normally; it introduces no dependency override or metadata patch.
All admitted wheels retain official PyPI hashes. The complete graph resolves
for Windows x64 CPython 3.13, Linux x64 CPython 3.14 and macOS arm64 CPython 3.13,
with every other package version unchanged.

The Pillow update has actual clean-install and installed-consumer qualification
on Windows: valid PNG/JPEG decoding, malformed input rejection, a JPEG tile-size
regression, recovery after rejection and Crawl4AI's screenshot-stitching consumer.
The screenshot test replaces browser I/O with explicit doubles while executing
the installed image-processing implementation. These checks do not establish
fresh Linux or macOS runtime qualification for the changed Pillow wheel; those
platforms currently have dependency-resolution evidence only. The earlier
three-platform qualification does not transfer to new wheel bytes.

The stable NLTK asset passed a clean, normally resolved hash-locked Windows
installation, installed-source admission, nine model-artifact security and
positive-flow tests, ten download-boundary tests, and a real headless crawl of
the upstream NLTK site with robots checks and a bounded page timeout. Original
Python and license files also match the independently captured same-version PyPI
wheel. These checks qualify the observed consumers; they do not cover every
optional NLTK feature or establish fresh Linux or macOS runtime acceptance.

Retain raw reports and verify the exact pushed revision's dependency alerts and
workflows. Preceding three-platform results do not transfer to new wheel bytes.
A changed lock, successful package install or version string alone
does not close a finding. New source, environment or advisory information
requires reviewing the affected boundary; the old acceptance never transfers
to another dependency or source revision.
