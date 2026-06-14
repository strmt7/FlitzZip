# Security Code Scanning

SuperZip uses automatic GitHub security workflows adapted for a native Windows
C++ app. The workflows are source-controlled, SHA-pinned where GitHub Actions
are used, and default to read-only repository permissions.

## Automatic Workflows

- `.github/workflows/security-code-scanning.yml` runs on push, weekly schedule,
  and manual dispatch.
- `.github/workflows/greenbone-openvas-vulnetix.yml` runs a Greenbone/OpenVAS
  integration audit on push and pull request.
- `.github/workflows/greenbone-openvas-live.yml` runs the authorized live
  network scan on weekly schedule and manual dispatch because it requires
  private scanner infrastructure, a Vulnetix organization, and an approved
  target.
- `.github/workflows/dependency-review.yml` runs Dependency Review on pull
  request only.
- `.github/workflows/release.yml` is manual-only and runs release build,
  packaging, install smoke tests, repository security scan, and publication.
  Product releases are HIP-enabled and require the HIP SDK installer checksum
  secrets plus the WiX v7 EULA acknowledgement variable documented in
  `docs/release.md`.
- `.github/dependabot.yml` keeps GitHub Actions dependencies visible through
  Dependabot pull requests.

## Scanner Coverage

| Scanner | Purpose | Output |
| --- | --- | --- |
| CodeQL C++ | Archive parser, path handling, CLI, Win32 UI, HIP boundary | GitHub code scanning |
| CodeQL Actions | Workflow injection and Actions misuse | GitHub code scanning |
| actionlint | GitHub Actions schema and expression validation | Workflow check |
| zizmor | GitHub Actions security analysis through a hash-locked `requirements-*.txt` wheel install | SARIF upload |
| Trivy | Filesystem dependency, config, secret, and license scan | SARIF upload |
| Semgrep | Cross-language SAST through a digest-pinned scanner container | SARIF upload |
| DevSkim | Microsoft security anti-pattern scanning | SARIF upload |
| OSV Scanner | Known dependency vulnerability scan | GitHub code scanning |
| Grype | Independent filesystem dependency vulnerability scan | SARIF upload |
| Gitleaks | Full git history and working-tree secret scan | JSON artifact |
| TruffleHog | Independent full git history secret scan | JSONL artifact |
| Dependency Review | Blocks vulnerable dependency changes on PRs | PR-only workflow |
| OSSF Scorecard | Repository supply-chain security posture | SARIF upload |
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

## Greenbone/OpenVAS Secrets

Greenbone/OpenVAS is a real network scanner. It needs an authorized target and
a reachable Greenbone GMP endpoint. Do not point it at systems you do not own
or have explicit permission to scan.

Required repository secrets:

- `GREENBONE_HOST`: Greenbone/GVM host reachable from GitHub Actions.
- `GREENBONE_USERNAME`: Greenbone user with permission to create targets,
  create tasks, start tasks, read reports, and delete temporary tasks.
- `GREENBONE_PASSWORD`: Password for `GREENBONE_USERNAME`.
- `GREENBONE_TARGET`: Authorized host, IP, or CIDR to scan by default.

Optional repository secrets:

- `GREENBONE_PORT`: GMP TLS port. Defaults to `9390`.
- `GREENBONE_SCAN_CONFIG_ID`: Scan config UUID. Defaults to the Greenbone
  "Full and fast" UUID used in the official scripting examples.
- `GREENBONE_SCANNER_ID`: Scanner UUID. Defaults to the OpenVAS scanner UUID
  used in the official scripting examples.
- `GREENBONE_PORT_LIST_ID`: Optional port-list UUID if your Greenbone setup
  requires an explicit list.
- `GREENBONE_MAX_MINUTES`: Maximum scan wait time. Defaults to `180`.
- `GREENBONE_DELETE_TASK`: Whether to delete the temporary scan task after
  report collection. Defaults to `true`.
- `VULNETIX_ORG_ID`: Vulnetix organization identifier for uploading OpenVAS
  artifacts after the scan.

Create the repository secrets through the GitHub UI:

1. Open `https://github.com/strmt7/SuperZip`.
2. Go to `Settings > Secrets and variables > Actions`.
3. Add each required value under `Repository secrets`.

Do not create a GitHub Actions environment for this workflow. SuperZip workflows
must never create deployment records, and scanner credentials must stay in
repository secrets.

Create the same repository secrets with GitHub CLI without exposing values in
shell history:

```powershell
gh secret set GREENBONE_HOST -R strmt7/SuperZip
gh secret set GREENBONE_USERNAME -R strmt7/SuperZip
gh secret set GREENBONE_PASSWORD -R strmt7/SuperZip
gh secret set GREENBONE_TARGET -R strmt7/SuperZip
gh secret set VULNETIX_ORG_ID -R strmt7/SuperZip
```

Each command prompts for the secret value. Do not pass secret values with
`--body` unless the command itself is run from a secure secret manager.

## Operational Rules

- The push and pull-request lane validates the workflow, hash-locked Greenbone
  tools, and GMP script contract without touching a network target.
- The scheduled/manual live scan lane fails closed with an explicit report when
  required Greenbone or Vulnetix secrets are absent. It does not claim a host
  scan succeeded.
- The live scan lane fails when Greenbone reports any critical, high, medium,
  or low vulnerability count.
- Pull requests from forks do not receive repository secrets. That is expected
  GitHub Actions behavior.
- Scanner scope must not be narrowed only to silence findings. Any exclusion
  needs a documented false-positive or generated-file rationale.
- `.semgrepignore` is intentionally present with comments only. It overrides
  Semgrep's default ignore list so tests and vendored code are scanned.
- Security findings should be fixed at the root cause. Suppressions are allowed
  only after the evidence is documented in this runbook or an issue.
- Workflows must never create GitHub deployment records. GitHub Actions
  `environment:` blocks and `deployment:` keys are forbidden by local security
  checks.
- Refresh GitHub Security tab alerts after every security remediation push.
