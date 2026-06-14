# Security Policy

## Supported Versions

Security fixes target the current `main` branch until versioned releases are
created.

## Reporting

Do not open public issues for exploitable archive parsing, path traversal,
malware scanning, credential, or supply-chain findings. Report privately through
GitHub's private vulnerability reporting if it is enabled for the repository, or
contact the repository owner through their preferred private channel.

## Scope

Security-sensitive SuperZip areas include:

- Archive metadata parsing and payload bounds checks.
- Extraction path validation.
- Overwrite behavior.
- AMD HIP runtime loading and GPU fallback behavior.
- Microsoft Defender opt-in scan handling.
- SHA-256 integrity hashing.
- CI workflows and repository secrets.

## Maintainer Checklist

Before a security release:

1. Add or update a regression test.
2. Run `tools\build.ps1 -Configuration Release`.
3. Run `tools\test.ps1 -Configuration Release`.
4. Run `tools\security_scan.ps1`.
5. Verify no credentials or local paths are present in commits.
