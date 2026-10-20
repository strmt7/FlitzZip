# Security Policy

## Supported Versions

Security fixes target the current `main` branch until versioned releases are
created.

## Reporting

Do not open public issues for exploitable archive parsing, path traversal,
malware scanning, credential, or supply-chain findings. Report privately through
GitHub private vulnerability reporting:
[Create a private report](https://github.com/strmt7/FlitzZip/security/advisories/new).

## Scope

Security-sensitive FlitzZip areas include:

- Archive metadata parsing and payload bounds checks.
- Extraction path validation.
- Overwrite behavior.
- AMD HIP runtime admission, GPU resource lifetime and required-HIP routing.
- Microsoft Defender opt-in scan handling.
- SHA-256 integrity hashing.
- CI workflows and repository secrets.
- Greenbone/OpenVAS and Vulnetix security automation.

## Maintainer Checklist

Before a security release:

1. Repair the affected production boundary and preserve a reproducer, legitimate
   controls and direct-consumer regression tests.
2. Select the complete change range with `tools\verification_plan.ps1` and run
   its required checks through `tools\verify_changes.ps1`. Product validation
   uses HIP-enabled binaries; unchanged evidence may be reused only when its
   source, configuration, tool and output identities match.
3. Complete the exact-commit hosted workflow wait and post-push audit described
   in [targeted verification](docs/targeted-verification.md). Resolve actionable
   source and dependency findings; historical dismissals do not establish a fix.
4. Qualify the exact portable ZIP and per-machine MSI, including runtime
   admission, checksums, corresponding source and installer install/repair/remove
   behavior, under [the release contract](docs/release.md).
5. Record remaining limitations and obtain maintainer approval before publication.
   Keep credentials, private paths and generated evidence out of commits.

An immutable Codex Security report describes its scanned revision. A successful
repair does not automatically update that report's displayed finding status.
Retain the original report and bind remediation evidence to the repaired commit;
use the service's supported verification or closure controls when available.
Never rewrite a sealed report to make an earlier finding disappear.

## Automation

See [security scanning](docs/security-code-scanning.md) for the automatic GitHub
scanner matrix and OIDC broker configuration. The
`greenbone-openvas-vulnetix` workflow checks integration contracts offline.
`greenbone-openvas-live` requires a separately authorized network target and
broker; an absent configuration is not a successful live scan. FlitzZip has no
declared network-service release target, so that optional infrastructure scan
is not a desktop release prerequisite.
