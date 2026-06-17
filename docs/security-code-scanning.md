# Security Code Scanning

SuperZip uses automatic GitHub security workflows adapted for a native Windows
C++ app. The workflows are source-controlled, SHA-pinned where GitHub Actions
are used, and default to read-only repository permissions.

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
  linting. Push and pull-request runs are change-aware; manual dispatch
  rechecks the latest commit range.
- `.github/workflows/dependency-review.yml` runs Dependency Review on pull
  request only.
- `.github/workflows/release.yml` is manual-only and runs release build,
  packaging, install smoke tests, repository security scan, and publication.
  Product releases are HIP-enabled and require the HIP SDK installer URL,
  checksum, and WiX v7 EULA acknowledgement variables documented in
  `docs/release.md`.
- `.github/dependabot.yml` keeps GitHub Actions and hash-locked Python scanner
  dependencies visible through Dependabot pull requests.

## Scanner Coverage

| Scanner | Purpose | Output |
| --- | --- | --- |
| CodeQL C++ | Whole-repository manual Windows build analysis of archive parser, path handling, CLI, Win32 UI, and HIP boundary | GitHub code scanning |
| CodeQL Actions | Workflow injection and Actions misuse | GitHub code scanning |
| Language linters | C/C++ style drift, PowerShell warnings, Python helper lint/format issues, YAML workflow issues, Markdown issues, and CMake style problems | Workflow check |
| actionlint | GitHub Actions schema and expression validation | Workflow check |
| zizmor | GitHub Actions security analysis through a hash-locked `requirements-*.txt` wheel install | SARIF upload |
| Trivy | Filesystem dependency, config, secret, and license scan | SARIF upload |
| Semgrep | Cross-language SAST through a hash-locked `requirements-*.txt` wheel install with full repository scan scope | SARIF upload |
| DevSkim | Microsoft security anti-pattern scanning through the pinned `Microsoft.CST.DevSkim.CLI` .NET tool | SARIF upload |
| OSV Scanner | Known dependency vulnerability scan | GitHub code scanning |
| Grype | Independent filesystem dependency vulnerability scan | SARIF upload |
| Gitleaks | Full git history and working-tree secret scan | JSON artifact |
| TruffleHog | Independent full git history secret scan | JSONL artifact |
| Dependency Review | Blocks vulnerable dependency changes on PRs | PR-only workflow |
| OSSF Scorecard | Default-branch repository supply-chain security posture | SARIF upload |
| Greenbone/OpenVAS | Always-on scanner integration audit plus scheduled/manual network vulnerability scan through hash-locked `requirements-*.txt` GVM tools for authorized targets | XML/JSON artifact and Vulnetix upload |
| Vulnetix | External vulnerability-management upload for authorized live OpenVAS results | Vulnetix project |

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
  "vulnetix_org_id": "redacted"
}
```

Required returned fields:

- `greenbone_host`: Greenbone/GVM host reachable from GitHub Actions.
- `greenbone_username`: Greenbone user with permission to create targets,
  create tasks, start tasks, read reports, and delete temporary tasks.
- `greenbone_password`: Password for `greenbone_username`.
- `greenbone_target`: Authorized host, IP, or CIDR to scan by default. A
  manual `workflow_dispatch` target overrides this value for one run.
- `vulnetix_org_id`: Vulnetix organization identifier for uploading OpenVAS
  artifacts after the scan.

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
- The only acceptable open code-scanning alerts are the current residual OSSF
  Scorecard findings for `MaintainedID`, `CodeReviewID`, `BranchProtectionID`,
  and `CIIBestPracticesID`. `BinaryArtifactsID`, SAST alerts, dependency
  alerts, secrets, and scanner policy findings must be fixed.
- After every push that changes security, workflows, dependencies, packaging, or
  release artifacts, run:

  ```powershell
  tools\github_post_push_audit.ps1
  ```

  The audit checks that no GitHub deployment records exist and that open
  code-scanning alerts are limited to the approved residual Scorecard findings.
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
