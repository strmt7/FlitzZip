# Semgrep Verified Download Reviews

These are two individually reviewed reports from Semgrep OSS 1.181.0. Their
exact source, scanner, location, callers, provenance and tests are bound in the
current review ledger. Raw reports remain available. Changed identities require
a new review; these decisions do not admit any other URL finding.

## Checked source context

The Git-backed review reader verifies bounded regular blobs and complete source
hashes. Its blob queries pass `check=True` directly; a failing query cannot supply
plausible bytes for admission. The context comparison explicitly permits Git
diff's ordinary exit codes zero and one, rejects other failures and accepts only
zero. Making these call arguments explicit preserves the preceding checked-query
and unequal-context behavior. The downloaders, URL admission, integrity checks,
reported locations and scanner recipe remain unchanged. Both individual decisions
below retain their complete context lists and the refreshed evidence binding.

The review reader now resolves Git to an absolute native executable before each
query. The shared resolver rejects relative command paths and implicit
current-directory search. Real Git inventory and shadow-file regressions qualify
that boundary, and both individual contexts include the resolver and its tests.
The downloader sources, URL restrictions, digest checks and finding locations
remain unchanged.

The identified Crawl4AI build now records the download handle's optional
ownership type and types the extracted tar-member list. The handle change is
annotation-only; validation, transfer and cleanup operations are unchanged.
The new downstream version and wheel digest identify those source bytes. The
upstream HTTPS URL, archive digest, length budget and redirect behavior are
unchanged. Both transport decisions were reviewed against the new context;
the downloader regression suite still exercises malformed URLs and payloads.

## Individual findings

### Alert 8446

**Pinned scanner asset transport.** Semgrep reports dynamic `urllib` use in
`tools/bootstrap_scanners.py`. The reported operation receives a URL constructed
only by the Gitleaks and DevSkim acquisition callers from their
version-validated, committed package pins. Before opening it, the transport
requires HTTPS, an exact approved host (`github.com` or `api.nuget.org`), no
userinfo, port, query or fragment, and a bounded expected byte length. It then
compares the complete downloaded length and SHA-256 against the pin before any
archive is extracted or package installed. The direct regression rejects
`file://`, an unapproved host, extra bytes and a wrong digest; cache repair and
the isolated NuGet feed have separate consumer regressions. An arbitrary local
file URL cannot reach the reported call.

### Alert 8447

**Pinned Crawl4AI source transport.** The same Semgrep rule reports `urllib` in
`tools/crawl4ai_source_build.py`. `recipe()` admits only the exact published
PyPI HTTPS URL, source filename, version, byte length and build epoch recorded
for Crawl4AI 0.9.5. `source_archive()` calls that validator before constructing
the request. It verifies the complete length and independent SHA-256 sidecar
before admitting the source to the local tool cache or extracting a member.
The direct regressions reject a changed recipe URL, incomplete and excess
responses, wrong digest and corrupted cache contents; the real published
source and repaired-wheel build were separately qualified. Neither users nor
webpage content control the URL. The reported local-file-scheme concern does
not apply to this pinned acquisition path.
