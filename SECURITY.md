# Security Policy

## Supported Versions

Security fixes target the current `main` branch until versioned releases are
created.

## Reporting

Do not open public issues for exploitable archive parsing, path traversal,
malware scanning, credential, or supply-chain findings. Report privately through
GitHub private vulnerability reporting:
https://github.com/strmt7/SuperZip/security/advisories/new

## Scope

Security-sensitive SuperZip areas include:

- Archive metadata parsing and payload bounds checks.
- Extraction path validation.
- Overwrite behavior.
- AMD HIP runtime loading and GPU fallback behavior.
- Microsoft Defender opt-in scan handling.
- SHA-256 integrity hashing.
- CI workflows and repository secrets.
- Greenbone/OpenVAS and Vulnetix security automation.

## Maintainer Checklist

Before a security release:

1. Add or update a regression test.
2. Run `tools\build.ps1 -Configuration Release` on a HIP-capable Windows host.
3. Run `tools\test.ps1 -Configuration Release`.
4. Run `build\Release\superzip_cli.exe dependency-check`.
5. Run `tools\security_scan.ps1`.
6. Verify the GitHub `security` and `greenbone-openvas-vulnetix` workflows.
7. Verify no credentials or local paths are present in commits.

## Automation

See `docs/security-code-scanning.md` for the automatic GitHub security scanner
matrix, Greenbone/OpenVAS secret setup, and Vulnetix upload configuration.
