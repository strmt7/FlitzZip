# Development Dependency Remediation

The earlier temporary acceptance of
[NLTK advisory GHSA-8mgp-746c-j5xp](https://github.com/advisories/GHSA-8mgp-746c-j5xp)
applied to the isolated development crawler's previous dependency pin. The
crawler now uses the unmodified official wheel from
[v3.10.4-rc1](https://github.com/nltk/nltk/releases/tag/v3.10.4-rc1), whose package
metadata reports version `3.10.4`. This release candidate was specifically
authorized by the maintainer after qualification; it does not establish a
general policy of adopting prereleases or overriding dependency constraints.

The [upstream distribution record](../../third_party/upstream/nltk/3.10.4-rc1/distribution.json)
binds exact asset bytes and original notices. The installer retrieves the wheel
into its external cache through normal pip HTTPS transport, verifies it before
atomic publication, and refuses corrupted cache entries. Built wheels are not
tracked in Git. Installed-source admission,
model-artifact failure and positive-flow regressions, selected upstream
path-security suites and real crawler consumers qualify the candidate.
Platform and optional-feature skips remain explicit. The locally retained
upstream wheel/source comparison and test receipts are evidence for that exact
candidate, not a claim that every optional NLTK feature has been tested.

Aiohttp is pinned to `3.14.4`, the patched version for
[GHSA-5wr8-gcqf-gqf3](https://github.com/advisories/GHSA-5wr8-gcqf-gqf3),
[GHSA-494j-54xm-rwp3](https://github.com/advisories/GHSA-494j-54xm-rwp3) and
[GHSA-2g87-pp6c-29x5](https://github.com/advisories/GHSA-2g87-pp6c-29x5).
The complete crawler graph resolves normally. The upstream security regressions
exercise initial-connection deadlines, Secure cookies and bounded parser,
WebSocket, cookie and decompression behavior.

The updated graph passed clean installation, installed API contracts, browser
extraction and cache reuse on Windows, macOS and Linux. Website qualification
retains both successful extraction and access failures; a timeout or a site
challenge is not reported as a successful crawl. The exact pushed dependency
audit reports the preceding alerts fixed, without manual dismissal.

Retain raw reports and verify the exact pushed revision's dependency alerts and
workflows. A changed lock, successful package install or version string alone
does not close a finding. New source, environment or advisory information
requires reviewing the affected boundary; the old acceptance never transfers
to another dependency or source revision.
