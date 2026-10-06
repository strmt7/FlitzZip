# Security Code Scanning

SuperZip uses automatic GitHub security workflows adapted for a native Windows
C++ app. The workflows are source-controlled, SHA-pinned where GitHub Actions
are used, and default to read-only repository permissions.

## Scanner Input Roles

Folder names alone do not establish security relevance. Product code, tests,
build-time source templates, scanner configuration and acquisition manifests
remain security inputs wherever they live. Secret/privacy detectors still
cover passive documents and measurements. Measurement producers and plotting
scripts belong in code directories; passive reports belong in the validated
data directory. New executable or unknown files there fail role admission
instead of inheriting a blanket directory exception.

Historical October 4 source dispositions are recorded in
`.github/scanner-source-reviews.csv` and the
[individual review](security-source-finding-review-2026-10-04.md). The
[SDK copy supplement](security-sdk-finding-review-2026-10-04.md) records the
six separately approved fixed-width byte-access operations. The actual
detectors still scan those files. Historical matching recognizes only each
exact source hash, rule and complete line/column region. Raw reports and counts
stay complete. The informational review tool also checks analysis identity.
These records are historical review evidence. They no longer admit source
findings to publication or security acceptance. Every source report counts as
unresolved in the preflight, including an exact historical match. The hosted
audit requires closure of every open alert and never consults approval ledgers.

The [remaining individual review](security-remaining-finding-review-2026-10-04.md)
records 54 separately approved reports in `.github/scanner-hosted-reviews.csv`.
Their complete incident identities use the public integrity pin in
`.github/scanner-hosted-approval.csv`, independently from caller-context
renewal. That context includes native build inputs, all generated Zstandard
library sources and CodeQL configuration. Changed source, caller context,
producer or coordinates invalidate a historical match. Matching a record is
informational and cannot establish remediation. Do not reseal these ledgers to
make a changed implementation pass. Follow the
[source remediation redesign](security-source-remediation-redesign.md).

### Prevention Strategy and Research

The pre-publication changed-function gate covers edited vendor C/C++, both brace
styles, complete contracts and long-function body documentation. It operates on
actual changed functions; it does not suppress queries or replace native tests.
The scanner preflight freezes changed publication bytes and uses the pinned
detectors. Hosted acceptance still requires the exact analyzed commit.

GitHub documents that [stable file paths and SARIF fingerprints](https://docs.github.com/en/code-security/reference/code-scanning/sarif-files/sarif-support)
prevent duplicate alerts across runs. Preserve paths, rule IDs, categories and
fingerprints when modifying scanners; never invent fingerprints to hide results.
GitHub's [incremental-analysis guidance](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/scan-from-the-command-line/incremental-analysis)
states that CodeQL Actions handle incremental analysis automatically, and CLI
overlays require no-build extraction. SuperZip retains its traced native build
and generated dependency coverage; switching to no-build extraction or filtering
results to edited lines would not provide equivalent acceptance evidence here.
The existing [security-and-quality suite](https://docs.github.com/en/code-security/concepts/code-scanning/codeql/codeql-query-suites)
retains both security and maintainability checks. Microsoft's
[native analysis guidance](https://learn.microsoft.com/en-us/cpp/code-quality/using-the-cpp-core-guidelines-checkers?view=msvc-170)
supports automated type/resource checks, but adding another analyzer is not
evidence that existing findings were repaired. Retain the pinned working tools
and their production regressions before adding new overlapping diagnostics.

`docs/benchmarks/data/` contains passive measurement reports only. DevSkim's
code-pattern analysis excludes that whole directory after
`tools/devskim_scope.py` validates its role. The shared local/hosted boundary
rejects source files, test files, unknown record kinds, malformed JSON,
corpus acquisition configuration, reparse redirects and resource overruns.
This is an input contract, not a rule suppression or a finding filter.
The real pinned matcher contract verifies that source, tests, configuration,
`bin/` code and nested lookalike paths retain findings, including checkout names
with punctuation. JSON options preserve array boundaries. The pinned matcher's
single-character placeholders apply only to its already selected absolute
scan-root prefix; fixed directory depth and the literal report suffix keep
neighboring inputs outside the exclusion.

Corpus download descriptors live in `docs/benchmarks/corpora/` and remain
scanned. Production source, vendored compiled source, test code and fixtures,
tools, workflows and other configuration remain in code analysis. Gitleaks
and TruffleHog still scan report data and Git history; generated metadata can
contain leaked credentials and therefore has no folder-level secret exemption.
The existing exact public-checksum exceptions retain field/value constraints.

Local preflight retains every raw finding. Its metadata review ledger records
only individually approved public integrity matches in corpus descriptors, Gitleaks
policy, the benchmark-permission manifest and the development-notice manifest.
The typed contract also recognizes public Git provenance in an exact NLTK
source-build recipe. A commit review must bind the path, JSON project and commit
fields, original source archive name and official NLTK codeload URL to the same
value. The existing checksum-only CSV remains supported; typed records distinguish
SHA-256 values from Git commit identifiers.
A model-artifact commit review additionally binds the exact corpus or permission
manifest, the complete reviewed file hash and the highlighted public commit.
The corpus role requires the Pythia14M inert-file descriptor, original
`model.safetensors` name and official EleutherAI immutable artifact URL. The
permission role requires that same sole reviewed version and the official
EleutherAI and Hugging Face evidence URLs. These roles require individual
authorization; recognition alone does not activate a disposition.
A verdict requires the exact metadata path, rule, highlighted value and complete
reviewed file hash. Source and test paths cannot enter this ledger; changed
contents and other findings remain blocking. CSV keeps the review records
readable without changing the scanned source values. Recognizing a metadata role
does not authorize a review record.
This local admission does not dismiss hosted alerts or qualify release security.

A successful analysis with the same DevSkim category can update obsolete
findings automatically. Final acceptance requires the hosted result and fresh
alert audit; a local scope check alone does not prove closure. Earlier manual
metadata dispositions remain visible in GitHub's audit history.

## Automatic Workflows

- `.github/workflows/security-code-scanning.yml` runs source and dependency
  security scans on push, weekly schedule, and manual dispatch.
- `.github/workflows/scorecard.yml` runs OSSF Scorecard on the default branch,
  weekly schedule, and manual dispatch. Scorecard is isolated because the action
  rejects non-default branch push refs.
- `.github/workflows/greenbone-openvas-vulnetix.yml` runs a Greenbone/OpenVAS
  integration audit on push and pull request.
- `.github/workflows/greenbone-openvas-live.yml` runs the authorized live
  network scan on weekly schedule and manual dispatch because it requires
  private scanner infrastructure, a Vulnetix organization, and an approved
  target.
- `.github/workflows/lint.yml` runs language linters for the languages used in
  this repository: C/C++ formatting, PowerShell static analysis, Python helper
  lint/format checks, YAML workflow linting, Markdown linting, and CMake
  linting. Push and pull-request runs are change-aware; weekly and manual runs
  check all owned files. Markdown includes root documents and agent skills,
  not only README and `docs/`. Upstream vendor formatting is not rewritten to
  enforce SuperZip style; its compiled/source security checks remain active.
- `.github/workflows/dependency-review.yml` runs Dependency Review on pull
  request only.
- `.github/workflows/release.yml` is manual-only and runs release build,
  packaging, install smoke tests, repository security scan, and publication.
  Product releases are HIP-enabled and use the complete SDK pinned in
  `tools/rocm-sdk-lock.json`. The WiX v7 EULA acknowledgement variable remains
  required as documented in `docs/release.md`.
- `.github/dependabot.yml` keeps GitHub Actions and hash-locked Python scanner
  dependencies visible through Dependabot pull requests.

## Scanner Coverage

| Scanner | Purpose | Output |
| --- | --- | --- |
| CodeQL C++ | Manual Windows CPU-only build analysis of compiled archive parsers, path handling, CLI, Win32 UI, host GPU boundary, and vendored C/C++; not HIP device-kernel extraction | GitHub code scanning |
| CodeQL Actions | Workflow injection and Actions misuse | GitHub code scanning |
| Language linters | C/C++ style drift, PowerShell warnings, Python helper lint/format issues, YAML workflow issues, Markdown issues, and CMake style problems | Workflow check |
| actionlint | GitHub Actions schema and expression validation | Workflow check |
| zizmor | GitHub Actions security analysis through a hash-locked `requirements-*.txt` wheel install | SARIF upload |
| Trivy | Filesystem dependency, config, secret, and license scan | SARIF upload |
| Semgrep | Cross-language SAST through a hash-locked install, unrestricted source scope, and same-run coverage/diagnostic evidence | SARIF upload and coverage artifact |
| DevSkim | Microsoft security anti-pattern scanning through a version/hash-verified `Microsoft.CST.DevSkim.CLI` package | SARIF upload |
| OSV Scanner | Known dependency vulnerability scan | GitHub code scanning |
| Grype | Independent filesystem dependency vulnerability scan | SARIF upload |
| Gitleaks | Full git history and working-tree secret scan | JSON artifact |
| TruffleHog | Independent full git history secret scan | JSONL artifact |
| Dependency Review | Blocks vulnerable dependency changes on PRs | PR-only workflow |
| OSSF Scorecard | Default-branch repository supply-chain security posture | SARIF upload |
| Greenbone/OpenVAS | Always-on scanner integration audit plus scheduled/manual network vulnerability scan through hash-locked `requirements-*.txt` GVM tools for authorized targets | XML/JSON/SARIF artifact and Vulnetix upload |
| Vulnetix | External vulnerability-management upload for authorized live OpenVAS results | Vulnetix project |

Semgrep produces JSON coverage evidence and SARIF in the same invocation. The
coverage artifact compares `paths.scanned` with **all** Git-tracked paths,
including deeply nested files, tests, vendor files, and agent skills; it does
not inherit an extension, directory, or depth exclusion from the old log-only
scope listings. Missing C/C++/Python/JavaScript/YAML source is a failing gate.
All other unscanned tracked files remain listed, not silently classified as
safe. Binary provenance archives, unsupported languages, and files admitted
with parser/matcher diagnostics need their appropriate independent review.
Admission is not successful parsing, sufficient rule coverage, or proof of
safety. The artifact retains diagnostic types and paths, but not diagnostic
messages, matched source snippets, or scanner-authentication material.

Inline Semgrep and DevSkim comment suppression is disabled; the empty `.semgrepignore` and
`--no-git-ignore` scope remain intact. The coverage tool's offline tests run
in both Windows CI and the security job. HIP-enabled product tests, kernel
source review, and release validation remain separate from hosted CPU-only
CodeQL. Do not describe the latter as device-kernel analysis.

DevSkim 1.0.100 was checked against the official stable GitHub release and
NuGet package on 2026-10-01. Its pinned package is downloaded with bounded
HTTPS-only curl, verified by exact size and SHA-256, then installed through a
local-only NuGet source. `.github/requirements/devskim-packaging.json` owns
the version, measured package size, and digest; `tools/devskim_provenance.py`
derives curl's byte ceiling from that same record. Offline tests cover invalid
metadata, truncation, growth, corruption, and streaming chunk boundaries.
An isolated empty NuGet cache and cleared fallback folders prevent reuse of
a different same-version package. The configuration follows
[NuGet's documented lookup order](https://learn.microsoft.com/en-us/nuget/reference/nuget-config-file#fallbackpackagefolders-section).

The pinned [upstream SARIF writer](https://github.com/microsoft/DevSkim/blob/ea92e6f3cc1a1482c39afbe2060aba7f77b74c48/DevSkim-DotNet/Microsoft.DevSkim.CLI/Writers/SarifWriter.cs)
emits empty rendered snippets when `--skip-excerpts` is active. Its serializer
omits the required rendered `text`, so GitHub rejects the report against
[SARIF 2.1.0](https://docs.oasis-open.org/sarif/sarif/v2.1.0/cos02/schemas/sarif-schema-2.1.0.json).
`tools/devskim_report.py` removes only these empty optional physical-region
snippets. It preserves every finding, location, message, severity, and
fingerprint; nonempty or unknown snippet content fails publication. Raw scanner
output stays in runner temporary storage, and only the completed report is
published atomically. Offline regressions cover metadata parity, malformed
reports, bounded resources, overwrite refusal, and publication failures.
GitHub's complete SARIF schema validation remains enabled. This adapter is
not a finding filter, and successful upload does not establish zero alerts.

The upstream generic hexadecimal-literal rule remains enabled;
upgrading the CLI does not establish that public-digest matches are secrets
or that all old findings were fixed. Locally defined scanner jobs have explicit time budgets;
CodeQL C++ retains 60 minutes, compared with 20 minutes for the observed
2026-10-01 build/analysis. A timeout is a failed gate, never clean-scan evidence.

## Finding Triage

The [individual Crawl4AI public-example review](security-crawl4ai-public-proxy-review-2026-10-05.md)
records the maintainer's exact authorization for four historical URI examples
and their public integrity identities. Its separate approval cohort preserves
the original NLTK ledger bytes and bindings. Metadata admission requires the
complete cohort hash and exact approved digest; source, location, value or
scanner changes expire the relevant review. Every sanitized raw finding is
retained, and no source-code vulnerability is closed by these dispositions.

The [6 October public-test review](security-crawl4ai-public-test-review-2026-10-06.md)
records four separately authorized parser/filter fixtures and their five public
integrity values. Its independent cohort preserves both earlier ledgers. The
consumer binds actual member/source locations separately from reported archive
attributions, and rejects verified credentials or any unmatched identity.

The maintainer approved the ten exact integrity metadata records in
[the 5 October review](security-public-checksum-review-2026-10-05.md). These
cover original corpus, comparator binary and license-notice hashes. Raw reports
remain intact, changed file bytes or values expire admission, and source/test
findings cannot use this metadata ledger. This approval does not dismiss hosted
alerts or close unresolved legal, source, GPU or release obligations.

The [NLTK public commit review](security-nltk-public-commit-review-2026-10-05.md)
documents one additional provenance report. Its status and individual maintainer
authorization are recorded in that review; the admission ledger cannot inherit
the earlier checksum approvals for this separate value.

The [latest individual review](security-finding-review-2026-10-03.md#latest-individual-metadata-triage)
records five metadata/namespace false-positive dispositions at `ed3ea0f`.
Each has its own source/producer evidence and GitHub audit comment. They are
not automatic closures or code-finding exemptions; all remaining source and
test findings stay in the review queue.

Keep all scanners enabled and retain their raw reports. A green workflow means
the scanner completed, not that GitHub has zero open alerts. Inspect alerts for
the exact pushed SHA and record whether each result is actionable, a verified
false positive, or unresolved. Do not alter application code or remove
reproducibility data just to reduce an alert count.

DevSkim rule `DS173237` matches quoted hexadecimal strings of at least 30
characters, including ordinary benchmark SHA-256 digests and commit IDs.
Before marking an individual result false positive, verify the highlighted
value and its structured field (`*_sha256` or `source_commit`) against the
record producer; historical Silesia `md5` metadata and pinned SHA-256 corpus
constants require the same provenance check. Require the Gitleaks
history/working-tree and TruffleHog history jobs to pass. An unfamiliar field, token-shaped surrounding
context, or failed secret scan remains open for investigation. Rule `DS137138`
can also flag the mandatory `http://www.w3.org/2000/svg` XML namespace;
validate the exact namespace and its use before triage. Never blanket-disable
either rule. Dismiss confirmed false positives with a specific GitHub audit
comment; do not dismiss unresolved findings or change the source to evade a
pattern.

The October 3 beta records contain two source-file SHA-256 fields whose names
include `sdk_byte_access`. Gitleaks' generic API-key detector reported 32
occurrences. Each digest matches both its recorded Git blob and current source
bytes. The maintainer authorized exclusions for verified metadata only, with
actual code findings retained. `.github/gitleaks.toml` extends every default
rule and admits only those two exact filename/digest pairs within the guarded
passive report directory, without date-specific filenames. Changed values,
authentication fields, extra same-line
content and unrelated paths remain detectable. The pinned 8.30.1 scanner passed
the complete local history with this policy and five real scanner boundary
checks; hosted history and working-tree acceptance still require a new run.
DevSkim uses the separate input-role contract above; this checksum policy does
not alter DevSkim rules or suppress any code findings.

On October 3, 2026, [alert 1742](https://github.com/strmt7/SuperZip/security/code-scanning/1742)
was confirmed to match the public ROCm archive SHA-256 pin, which
`download_archive` verifies against archive bytes. [Alert 1743](https://github.com/strmt7/SuperZip/security/code-scanning/1743)
matched HTTP metadata in a negative redirect test; `NoRedirect.redirect_request`
unconditionally raises before network I/O, while production downloads require
HTTPS. Both dispositions reviewed source at `be30316` and required the passing
Gitleaks history/working-tree and TruffleHog history steps in
[security run 37100463323](https://github.com/strmt7/SuperZip/actions/runs/37100463323).
Each alert has its own evidence comment. No rule, scan scope or test was removed.

On October 3, [alert 1754](https://github.com/strmt7/SuperZip/security/code-scanning/1754)
and [alert 1753](https://github.com/strmt7/SuperZip/security/code-scanning/1753)
were individually confirmed to match the pinned `archive_sha256` and
`member.sha256` in `household-power-source.json` at `5c1101f`. These are locally
observed public dataset digests, not credentials or publisher-signed hashes.
The actual acquisition authenticated the complete 20,640,916-byte official ZIP
and its 132,960,755-byte member, including complete-member CRC. The producer in
`tools/acquire_benchmark_corpus.py` compares complete bytes against these fields
before writing excerpts. Gitleaks history/working-tree and TruffleHog history
passed for the exact SHA in
[run 37114057563](https://github.com/strmt7/SuperZip/actions/runs/37114057563).
Both alerts have specific false-positive comments within GitHub's 280-character
limit; the initial oversized comment was rejected before changing any alert.
The subsequent audit still reported 209 unapproved open alerts. No broader
disposition, scanner exclusion or source-pin removal followed this review.

## Scanner Dependency Remediation

On 2026-09-30, OSV and Grype reported PyJWT 2.13.0 in
`.github/requirements/requirements-semgrep-linux.txt`. The affected package is
CI scanner tooling, not a dependency shipped in the SuperZip application.
[PyJWT's newer pre-verification advisory](https://github.com/advisories/GHSA-42vr-xj54-vc7v)
requires at least 2.15.0; the latest stable PyJWT checked on 2026-10-01 is 2.15.1.
At that point Semgrep 1.178.0 declared `pyjwt[crypto]~=2.13.0`, excluding that version.

The Linux CPython 3.14 target was checked with uv's dependency resolver:
`semgrep==1.178.0` together with a patched PyJWT is unsatisfiable. The
[upstream constraint issue](https://github.com/semgrep/semgrep/issues/11925)
remains open. The maintainer explicitly authorized compatibility investigation
and production promotion on 2026-10-01. An isolated, non-root CPython 3.14.7
Linux run compared the original dependency graph with PyJWT 2.15.1. Normal
installation and `pip check` passed for the baseline; the experimental child
upgrade correctly reported the published metadata conflict. Runtime controls
passed for both: signed HMAC/RSA decode, invalid signatures/expiry, the actual
Semgrep MCP token-verifier call sites with local JWKS, CLI positive/negative
controls and SARIF output. No credentials or live authentication service were used.

The paired repository scans used 1,086 frozen `p/default` and `p/github-actions`
rules and the same 638-file working-tree snapshot. Both reported the same 619
scanned paths, zero Semgrep findings and 71 identical parsing/matching diagnostics.
These diagnostics are coverage limits, not proof of a vulnerability-free tree.
Other scanners and all current GitHub alerts remain required.

The October 1 production workflow built the explicit downstream distribution
`semgrep==1.178.0+superzip.1` through a reviewed wheel builder. The
official upstream wheel and its RECORD are verified before transformation.
Every code/notice byte stays unchanged; the distribution version and requirement
become the reviewed local identity and `pyjwt[crypto]==2.15.1`. Complete RECORD
hashes and dist-info paths are rebuilt. The hash-locked install uses normal
dependency resolution, then `pip check` and `tools.test_semgrep_runtime`, before
the unchanged full scan. There is no production dependency-check bypass,
installed-metadata edit, scanner removal or alert suppression. The native app
does not acquire a Python dependency.

The local production-path execution on CPython 3.14.7 passed normal hashed
installation, `pip check`, all five runtime tests and the full frozen-rule scan:
625 scanned paths, zero findings and the same 71 known diagnostics. Windows
CPython 3.12.14 and Linux CPython 3.14.7 produced the identical derived wheel
SHA-256 recorded in the lock. Eight offline packaging/configuration tests cover
payload and notice parity, complete RECORD hashes, repeatability, corrupted
inputs, metadata drift, collision refusal and the actual production workflow.

The advisory regression follows
[PyJWT's deterministic cross-version test](https://github.com/jpadilla/pyjwt/commit/9bc06658f875b9b40091539140bbbdc4639161c3),
not an assumed interpreter nesting limit. A differential control reproduced
uncaught `RecursionError` for PyJWT 2.13.0 and contained `DecodeError` for 2.15.1
through both direct pre-verification decode and the JWKS consumer, with no
network access. A live OSV query on 2026-10-01 returned no advisories matching
PyJWT 2.15.1. This is local package/runtime evidence, not GitHub alert closure.

On October 2, official Semgrep 1.179.0 changed its declaration to
`pyjwt[crypto]>=2.15.0,<3`. The current production workflow therefore uses that
unmodified official release with PyJWT 2.15.1. Its complete hash lock resolves
for Linux CPython 3.14; non-root Linux CPython 3.14.7 installation, `pip check`
and all five runtime tests pass without dependency overrides. The obsolete
wheel builder, local distribution manifest and `--find-links` are removed.
Offline installation tests retain input/lock parity, per-dependency hashes and
no-bypass workflow contracts. Coverage audits, runtime controls and the full
scanner remain mandatory; no scanner, rule or alert is suppressed by this update.

The official-release scan used a new 677-file frozen working-tree snapshot and
the current registry configurations: 1,075 loaded rules, 493 applicable rules,
658 admitted files, zero findings and valid SARIF 2.1.0. The complete inventory
audit reports no unscanned source paths and 43 parsing/matching diagnostics.
Remaining non-source omissions are binary artwork and provenance archives;
compiled dependency coverage belongs to the separate build-traced analysis.
The changed snapshot and registry contents prevent treating these numbers as a
controlled improvement over the earlier 71 diagnostics. Admission and zero
findings do not prove complete parsing or absence of vulnerabilities.
An October 2 OSV batch query returned no matching advisories for all 66 packages
in the refreshed scanner lock. All 31 full-profile local checks also pass; these
results do not establish hosted workflow or alert acceptance.

Local Linux verification uses the same installation and runtime commands as CI:

```sh
python -m unittest tools.test_semgrep_installation
python -m venv /tmp/superzip-semgrep
/tmp/superzip-semgrep/bin/python -m pip install --require-hashes --only-binary=:all: -r .github/requirements/requirements-semgrep-linux.txt
/tmp/superzip-semgrep/bin/python -m pip check
/tmp/superzip-semgrep/bin/python -m unittest tools.test_semgrep_runtime
```

Hosted remediation is not confirmed until the exact pushed commit passes its
scanner workflows and the post-push audit shows the affected alerts fixed.
The earlier downstream revision remains historical evidence, not the current
installation or an official release. CI tooling does not add a Python dependency
to SuperZip's native runtime.

## Required GitHub Repository Settings

Enable these in `strmt7/SuperZip`:

1. `Settings > Code security and analysis > Code scanning`.
2. `Settings > Code security and analysis > Dependabot alerts`.
3. `Settings > Code security and analysis > Secret scanning`.
4. `Settings > Code security and analysis > Private vulnerability reporting`.
5. Branch protection or rulesets requiring `windows-ci`, `security`, and
   `Greenbone/OpenVAS integration audit` checks before protected-branch updates.
   The `scorecard` workflow is repository-level and runs on the default branch,
   not on pull-request refs.

## Greenbone/OpenVAS OIDC Broker

Greenbone/OpenVAS is a real network scanner. It needs an authorized target and
a reachable Greenbone GMP endpoint. Do not point it at systems you do not own
or have explicit permission to scan.

Direct GitHub repository secrets are intentionally not used by the live scan
workflow. Zizmor correctly requires secret-consuming jobs to use dedicated
GitHub Actions environments, and Actions environments create deployment records.
Deployment records are forbidden in this repository, so private scanner
credentials must be resolved through an external OIDC broker.

Required repository variables:

- `GREENBONE_SECRET_PROVIDER_URL`: HTTPS endpoint for the scanner configuration
  broker. The broker must validate the GitHub OIDC token claims before returning
  any private scanner settings.
- `GREENBONE_SECRET_PROVIDER_AUDIENCE`: Optional OIDC audience. Defaults to
  `superzip-openvas`.

The broker must return this JSON object after validating the OIDC token:

```json
{
  "greenbone_host": "scanner.example",
  "greenbone_username": "superzip-ci",
  "greenbone_password": "redacted",
  "greenbone_target": "authorized-target.example",
  "greenbone_port": "9390",
  "greenbone_scan_config_id": "daba56c8-73ec-11df-a475-002264764cea",
  "greenbone_scanner_id": "08b69003-5fc2-4037-a479-93b440211c73",
  "greenbone_port_list_id": "",
  "greenbone_max_minutes": "180",
  "greenbone_delete_task": "true",
  "vulnetix_org_id": "<organization UUID>",
  "vulnetix_api_key": "<hexadecimal API key>"
}
```

Required returned fields:

- `greenbone_host`: Greenbone/GVM host reachable from GitHub Actions.
- `greenbone_username`: Greenbone user with permission to create targets,
  create tasks, start tasks, read reports, and delete temporary tasks.
- `greenbone_password`: Password for `greenbone_username`.
- `greenbone_target`: Authorized host, IP, or CIDR to scan by default. A
  manual `workflow_dispatch` target is a request for broker authorization, not
  an override. Only the broker-returned target becomes the effective scan target.
- `vulnetix_org_id`: Vulnetix organization UUID for uploading OpenVAS results.
- `vulnetix_api_key`: Hexadecimal API key used by the pinned action to
  authenticate and verify access before uploading the complete SARIF report.
  The resolver validates and masks both fields before emitting any outputs.

Optional returned fields:

- `greenbone_port`: GMP TLS port. Defaults to `9390`.
- `greenbone_scan_config_id`: Scan config UUID. Defaults to the Greenbone
  "Full and fast" UUID used in the official scripting examples.
- `greenbone_scanner_id`: Scanner UUID. Defaults to the OpenVAS scanner UUID
  used in the official scripting examples.
- `greenbone_port_list_id`: Optional port-list UUID if your Greenbone setup
  requires an explicit list.
- `greenbone_max_minutes`: Maximum scan wait time. Defaults to `180`.
- `greenbone_delete_task`: Whether to delete the temporary scan task and target
  after report collection or failure cleanup. Defaults to `true`.

Create the repository variables through the GitHub UI:

1. Open `https://github.com/strmt7/SuperZip`.
2. Go to `Settings > Secrets and variables > Actions`.
3. Add each required value under `Variables`.

Do not create a GitHub Actions environment for this workflow. SuperZip
workflows must never create deployment records, and scanner credentials must
stay outside GitHub Actions.

Create the same repository variables with GitHub CLI:

```powershell
gh variable set GREENBONE_SECRET_PROVIDER_URL -R strmt7/SuperZip --body https://scanner-broker.example/superzip/openvas
gh variable set GREENBONE_SECRET_PROVIDER_AUDIENCE -R strmt7/SuperZip --body superzip-openvas
```

The variable values are non-secret broker routing metadata. Do not put
Greenbone passwords, scanner targets, or Vulnetix organization IDs in GitHub
variables.

## Operational Rules

- The SuperZip lint stack is intentionally language-specific. It uses
  `clang-format` for owned C/C++ files, PSScriptAnalyzer for PowerShell, Ruff
  for Python helper scripts, yamllint for GitHub YAML, pymarkdownlnt for
  Markdown, and cmakelang for CMake. The Python/Docker reference repository
  pattern of separate Ruff/Mypy/Vulture/Super-Linter lanes is useful as a CI
  shape, but SuperZip does not run linters for languages or container surfaces
  that are not part of this Windows-native C++ product.
- CodeQL C++ uses `build-mode: manual` on `windows-2022` with
  `tools/build.ps1 -Configuration Release -CpuOnlyValidation`. Build-free C/C++
  analysis under-modeled SuperZip's Win32, GDI+, HIP-boundary, and vendored C
  sources and produced parser-artifact alerts such as namespace qualifiers,
  macros, and typed pointer arithmetic being reported as code issues. Manual
  Windows tracing is the default security signal because it uses real compiler
  inputs while keeping release artifacts HIP-enabled outside hosted CI.
- Do not split CodeQL C++ by SuperZip subdirectory. Subdirectory-parallel CodeQL
  can be useful for independent interpreted-language monorepos, but SuperZip is
  one C++ product whose archive parser, path validation, CLI, GUI, and GPU
  boundary share data flow. Partial C++ databases can hide cross-component
  vulnerabilities.
- The generated Win32 logo header is deterministic visual geometry validated by
  `tools\verify_brand_assets.ps1`; keep it covered by the manual Windows build
  database and do not add CodeQL path exclusions for generated source.
- Every open code-scanning alert blocks final security acceptance, including
  Scorecard governance reports. Source fixes require fresh analysis of the
  repaired revision. Governance and attestation reports require their actual
  controls; a code rewrite cannot establish them.
- After every push that changes security, workflows, dependencies, packaging, or
  release artifacts, run:

  ```powershell
  tools\github_post_push_audit.ps1
  ```

  The audit checks that no GitHub deployment records exist and that no
  code-scanning alerts remain open. It retains complete pagination and history.
- For a requested review of current and resolved incidents, run the same audit
  with `-IncludeHistory -HistoryReportPath out/security/history-review.json`.
  Use a new filename for each refresh; existing reports are never overwritten.
  It queries every state once, saves minimal incident evidence and rule/state
  totals, then applies the unchanged open-alert gate. An unresolved finding still
  fails the command after its report is saved. Ordinary post-push checks remain
  open-only to avoid repeatedly downloading the entire history.
  See [the review and recurrence map](code-scanning-history-review.md).
- The push and pull-request lane validates the workflow, hash-locked Greenbone
  tools, and GMP script contract without touching a network target.
- The scheduled/manual live scan lane fails closed with an explicit report when
  the OIDC broker is absent or omits required Greenbone/Vulnetix settings. It
  does not claim a host scan succeeded.
- The live scan lane fails when Greenbone reports any critical, high, medium,
  or low vulnerability count.
- Pull requests do not run the live network scan. The live lane is scheduled
  and manual-only.
- Scanner scope must not be narrowed only to silence findings. Any exclusion
  needs a documented false-positive or generated-file rationale.
- DevSkim runs through the .NET CLI package instead of the container action so
  security CI does not depend on building the action image from Microsoft
  Container Registry during every run. This is a scanner execution hardening,
  not a scan-scope reduction.
- `.semgrepignore` is intentionally present with comments only. It overrides
  Semgrep's default ignore list so tests and vendored code are scanned.
- Security findings should be fixed at the root cause. Suppressions are allowed
  only after the evidence is documented in this runbook or an issue.
- Workflows must never create GitHub deployment records. GitHub Actions
  `environment:` blocks and `deployment:` keys are forbidden by local security
  checks.
- Workflow `run` blocks must not interpolate `${{ github.* }}` directly. Put
  untrusted GitHub context values in `env:` and quote the environment variables
  inside the shell script. This is enforced locally by changed-file hygiene and
  repo-wide security scans.
- Refresh GitHub Security tab alerts after every security remediation push.
