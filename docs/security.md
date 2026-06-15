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

Regular file extraction uses a same-directory temporary target and publishes the
final file only after the entry has been decoded and verified. A corrupt ZIP,
TAR, or SUZIP entry can therefore fail after writing temporary bytes without
exposing a partial final file or replacing an existing final file.

TAR extraction is two-pass: SuperZip scans all headers and PAX path metadata,
validates the complete normalized path set, and only then creates output.
Symbolic links, hard links, devices, and FIFOs are rejected until a dedicated
Windows restore policy exists.

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

Compression can also opt in to `--verify-after-write`, which immediately reads
the completed `.suzip` archive through the normal verifier. This extra pass is
not implicit because it doubles read work for large archives and should be a
deliberate integrity policy choice.

The CLI exposes:

```powershell
build/Release/superzip_cli.exe compress --format suzip --verify-after-write --output archive.suzip path\to\folder
build/Release/superzip_cli.exe verify --sha256 archive.suzip
build/Release/superzip_cli.exe extract --defender-scan --output restored archive.suzip
build/Release/superzip_cli.exe extract --defender-scan --output restored archive.tar
```

The GUI Preferences page must keep SHA-256 hashing disabled by default and label
it as an extra integrity check, not encryption or malware scanning.

The CLI and GUI both keep Defender scans opt-in. When Defender actively scans a
source archive before extraction and does not report it as clean, extraction is
blocked. If Defender is unavailable, the result is reported but SuperZip does
not claim the target is clean.

GitHub Actions should run the local security script first. Optional external
upload and vulnerability management lanes are documented in
`.github/workflows/greenbone-openvas-vulnetix.yml`.

## Vulnetix / OpenVAS Future Lane

The Greenbone/OpenVAS and Vulnetix live workflow resolves scanner credentials
through an external OIDC broker, then uses a pinned `Vulnetix/cli` action after
a real authorized OpenVAS scan has produced artifacts. The current pin is:

- `uses: Vulnetix/cli@7b1ca5050e6d55a0fa29752b5e779018a5f2aa5f`
- `org-id: ${{ steps.config.outputs.vulnetix_org_id }}`
- optional `task: upload`
- optional `artifact-path: ./reports/`

The SuperZip workflow fails closed until `GREENBONE_SECRET_PROVIDER_URL` points
to a broker that validates GitHub OIDC claims and returns the authorized
Greenbone and Vulnetix settings. Do not add hard-coded organization IDs,
tokens, URLs, scan targets, or credentials to the repository.

Note: the action is pinned to the `v3.21.0` commit verified against the
annotated upstream tag on June 15, 2026. Re-check the upstream action before
rotating that pin.
