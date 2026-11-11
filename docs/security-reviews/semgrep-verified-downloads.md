# Semgrep Verified Download Reviews

These are two individually reviewed reports from Semgrep OSS 1.185.0. Their
exact source, scanner, location, callers, provenance and tests are bound in the
current review ledger. Raw reports remain available. Changed identities require
a new review; these decisions do not admit any other URL finding.

## Checked source context

Both reported downloader sources, callers' URL restrictions, byte budgets and
integrity checks are unchanged. The current scanner uses the reviewed Semgrep
1.185.0 packaging revision and its normally resolved MCP/OpenTelemetry cohort.
Original scanner source, native executables, permissions and notices remain
byte-identical to that upstream release.

Runtime qualification checks the Python package, packaged native engine, CLI,
JSON and SARIF identities. The environment's own script directory takes
precedence during native redispatch. An inherited older global CLI was detected
by the identity regression; its results were retained as failed qualification,
not accepted as results from the new scanner.

The actual new scanner reproduces both reported calls at the original locations.
Their source hashes and complete JSON/SARIF finding identities agree. Direct
download regressions retain malformed-URL, size, digest, redirected-cache and
failure-cleanup controls. Each decision binds the scanner recipe, lock, workflow,
runtime qualification, acquisition callers and their relevant tests.

The rebase preserves the exact MCP 1.30.0 repair and updates the coordinated
OpenTelemetry API/SDK/OTLP and instrumentation requirements to 1.45.2/0.66b2.
The ordinary Linux installation passes dependency consistency and the actual
MCP, JWT, hostile-header, telemetry-export and scanner-control regressions.
Neither downloader's source, URL admission, pinned inputs or callers change.
The full new scan retains the same parser/matcher diagnostic paths and counts
for tracked source as the preceding scanner. Those diagnostics are limitations,
not successful parsing; they remain in the raw and coverage artifacts.

The Git-backed review reader admits bounded regular blobs, checked Git queries
and complete source hashes. Its executable resolver rejects relative command
paths and implicit current-directory search. Nonzero comparison results never
establish equal context. Neither review admits another URL finding, incomplete
analysis or a different scanner/version. Hosted Linux execution and the exact
committed-context audit remain separate required evidence.

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

The crawler retains Playwright 1.65.0, the original source-build identity and
official NLTK 3.10.4. Its compatible dependency refresh updates file locking,
package metadata, Markdown, HTML selection and the existing OpenAI SDK cohort.
The SDK adds its normally resolved lazy-import dependency; no external LLM is
used for research or qualification. Normal installation, dependency consistency,
download/model consumers, the real headless browser and the existing website
cohort qualify the changed environment. Platform projections are resolution
evidence, not cross-host execution.

Source acquisition, recipe validation, bounded reads, digest verification and
wheel construction finish before browser provisioning or SDK consumers. They
use the same standard-library `urllib` transport and immutable recipe,
independently of these dependency changes. The scanner-asset transport in alert
8446 has unchanged source, pins, callers and tests. Both individual decisions
retain their reported sources, locations, rules and scanner identities.
