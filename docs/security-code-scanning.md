# Security Code Scanning

SuperZip uses automatic GitHub security workflows adapted for a native Windows
C++ app. The workflows are source-controlled, SHA-pinned where GitHub Actions
are used, and default to read-only repository permissions.

## Automatic Workflows

- `.github/workflows/security-code-scanning.yml` runs on push, pull request,
  weekly schedule, and manual dispatch.
- `.github/workflows/greenbone-openvas-vulnetix.yml` runs on push, pull
  request, weekly schedule, and manual dispatch.
- `.github/workflows/release.yml` is manual-only and runs release build,
  packaging, install smoke tests, repository security scan, and publication.
- `.github/dependabot.yml` keeps GitHub Actions dependencies visible through
  Dependabot pull requests.

## Scanner Coverage

| Scanner | Purpose | Output |
| --- | --- | --- |
| CodeQL C++ | Archive parser, path handling, CLI, Win32 UI, HIP boundary | GitHub code scanning |
| CodeQL Actions | Workflow injection and Actions misuse | GitHub code scanning |
| actionlint | GitHub Actions schema and expression validation | Workflow check |
| zizmor | GitHub Actions security analysis through a hash-locked Python wheel | SARIF upload |
| Trivy | Filesystem dependency, config, secret, and license scan | SARIF upload |
| Semgrep | Cross-language SAST through a digest-pinned scanner container | SARIF upload |
| DevSkim | Microsoft security anti-pattern scanning | SARIF upload |
| OSV Scanner | Known dependency vulnerability scan | GitHub code scanning |
| Dependency Review | Blocks vulnerable dependency changes on PRs | PR check |
| OSSF Scorecard | Repository supply-chain security posture | SARIF upload |
| Greenbone/OpenVAS | Network/host vulnerability scan through hash-locked GVM tools for authorized targets | XML/JSON artifact and optional Vulnetix upload |
| Vulnetix | Optional external vulnerability-management upload | Vulnetix project |

## Required GitHub Repository Settings

Enable these in `strmt7/SuperZip`:

1. `Settings > Code security and analysis > Code scanning`.
2. `Settings > Code security and analysis > Dependabot alerts`.
3. `Settings > Code security and analysis > Secret scanning`.
4. `Settings > Code security and analysis > Private vulnerability reporting`.
5. Branch protection or rulesets requiring `windows-ci`, `security`, and
   `greenbone-openvas-vulnetix` checks before protected-branch updates.

## Greenbone/OpenVAS Secrets

Greenbone/OpenVAS is a real network scanner. It needs an authorized target and
a reachable Greenbone GMP endpoint. Do not point it at systems you do not own
or have explicit permission to scan.

Required repository secrets:

- `GREENBONE_HOST`: Greenbone/GVM host reachable from GitHub Actions or a
  self-hosted runner.
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

Create secrets through the GitHub UI:

1. Open `https://github.com/strmt7/SuperZip`.
2. Go to `Settings > Secrets and variables > Actions`.
3. Open the `Secrets` tab.
4. Click `New repository secret`.
5. Enter one name and value at a time.

Create the same secrets with GitHub CLI without exposing values in shell
history:

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

- If Greenbone secrets are absent, the workflow writes an explicit skipped
  report and stays green. It does not claim a host scan succeeded.
- Pull requests from forks do not receive repository secrets. That is expected
  GitHub Actions behavior.
- Scanner scope must not be narrowed only to silence findings. Any exclusion
  needs a documented false-positive or generated-file rationale.
- Security findings should be fixed at the root cause. Suppressions are allowed
  only after the evidence is documented in this runbook or an issue.
- Refresh GitHub Security tab alerts after every security remediation push.
