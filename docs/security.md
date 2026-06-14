# Security Model

SuperZip treats archive contents as untrusted input.

## Extraction Boundary

Extraction rejects:

- Absolute, drive-rooted, and UNC paths.
- `..` traversal and paths that normalize outside the destination root.
- Windows-reserved device names such as `CON`, `NUL`, `COM1`, and `LPT1`.
- Path components with Windows-invalid characters or unsafe trailing spaces and
  dots.
- Malformed archive block metadata, payload offsets outside the archive, and CRC
  mismatches.
- Existing output files unless overwrite is explicitly enabled.

The app refuses to archive symbolic links in the current release. That avoids
ambiguous restore semantics and prevents link-based escape behavior.

## CI Security Gates

The repository ships local scripts for:

- Source secret pattern scanning.
- Binary and archive output exclusion checks.
- C++ test execution including malicious archive path tests.
- Dependency notice validation.

## Optional Microsoft Defender Scanning

SuperZip includes an opt-in Microsoft Defender hook for Windows 11 systems. It
uses the local `MpCmdRun.exe` command with path scanning and
`-DisableRemediation`, so SuperZip can ask Defender for a scan result without
silently deleting or quarantining user files from inside the app.

This setting must remain opt-in. It can be surfaced in Preferences for:

- Scan archives after creation.
- Scan extracted files after restore.
- Scan selected archive before extraction.

If Microsoft Defender is unavailable, disabled by policy, or returns an error,
SuperZip reports that state to the user instead of claiming the file is clean.

## Optional Integrity Hashing

SuperZip always stores and verifies CRC-32 for archive corruption detection.
Users can opt in to stronger SHA-256 integrity hashing for archive files or
post-extraction verification. This is intentionally separate from the mandatory
CRC path because SHA-256 costs extra I/O on large archives.

The CLI exposes:

```powershell
build/Release/superzip_cli.exe verify --sha256 archive.szip
build/Release/superzip_cli.exe extract --defender-scan --output restored archive.szip
```

The GUI Preferences page must keep SHA-256 hashing disabled by default and label
it as an extra integrity check, not encryption or malware scanning.

The CLI and GUI both keep Defender scans opt-in. When Defender actively scans a
source archive before extraction and does not report it as clean, extraction is
blocked. If Defender is unavailable, the result is reported but SuperZip does
not claim the target is clean.

GitHub Actions should run the local security script first. Optional external
upload and vulnerability management lanes are documented in
`.github/workflows/vulnetix-openvas.yml`.

## Vulnetix / OpenVAS Future Lane

The Vulnetix OpenVAS page requires JavaScript in a static fetch, so the
integration stub follows the public `Vulnetix/cli` GitHub Action documentation
checked during implementation. The action documents:

- `uses: Vulnetix/cli@a6fb5f993c6404f1d2d2c36542fed67aa2a7738d`
- `org-id: ${{ secrets.VULNETIX_ORG_ID }}`
- optional `task: upload`
- optional `artifact-path: ./reports/`

The SuperZip workflow keeps this lane manual/optional until repository secrets
and any external OpenVAS report producer are configured. Do not add hard-coded
organization IDs, tokens, URLs, scan targets, or credentials to the repository.

Note: the action is pinned to the commit observed on June 14, 2026. Re-check the
upstream action before rotating that pin.
