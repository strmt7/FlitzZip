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
silently deleting or quarantining user files from inside the app. Runtime lookup
prefers the newest versioned Defender Platform copy under ProgramData and falls
back to the Program Files copy only when the platform copy is unavailable.

This setting must remain opt-in. It can be surfaced in Settings for:

- Scan archives after creation.
- Scan extracted files after restore.
- Scan selected archive before extraction.

If Microsoft Defender is unavailable, disabled by policy, or returns an error,
SuperZip reports that state to the user instead of claiming the file is clean.
Enabled scans validate that the selected target exists before scanner discovery.
The scanner subprocess is hidden, bounded by a 30-minute timeout, and treated as
not clean if it times out.

## Optional Integrity Hashing

SuperZip always stores and verifies CRC-32 for archive corruption detection.
Users can opt in to stronger SHA-256 integrity hashing for archive files,
selected queue paths, and post-extraction output verification. Regular files
use standard SHA-256 over file bytes. Directories use a deterministic
SuperZip tree digest that hashes stable relative paths, file sizes, and file
payload bytes in sorted order; reparse points and unsupported entry types are
rejected instead of being silently skipped. This is intentionally separate from
the mandatory CRC path because SHA-256 costs extra I/O on large archives and
large output trees.

Compression can also opt in to `--verify-after-write`, which immediately reads
the completed `.suzip` archive through the normal verifier. This extra pass is
not implicit because it doubles read work for large archives and should be a
deliberate integrity policy choice.

The CLI exposes:

```powershell
build/Release/superzip_cli.exe compress --format suzip --verify-after-write --output archive.suzip path\to\folder
build/Release/superzip_cli.exe verify --sha256 archive.suzip
build/Release/superzip_cli.exe extract --sha256 --defender-scan --output restored archive.suzip
build/Release/superzip_cli.exe extract --defender-scan --output restored archive.tar
```

The GUI Settings page must keep SHA-256 hashing disabled by default and label
it as an extra integrity check, not encryption or malware scanning.
Compress, Extract, and Security use the same hashing path as the CLI: created
archives, input archives, selected queue paths, and restored output folders all
produce visible history rows when SHA-256 is enabled.

The CLI and GUI both keep Defender scans opt-in. When Defender actively scans a
source archive before extraction and does not report it as clean, extraction is
blocked. If Defender is unavailable, the result is reported but SuperZip does
not claim the target is clean.

GitHub Actions should run the local security script first. Optional external
upload and vulnerability management lanes are documented in
`.github/workflows/greenbone-openvas-live.yml`.

## Vulnetix / OpenVAS Future Lane

The Greenbone/OpenVAS and Vulnetix live workflow resolves scanner credentials
through an external OIDC broker. The [canonical live workflow](../.github/workflows/greenbone-openvas-live.yml)
contains the reviewed immutable `Vulnetix/cli` pin and matching upstream tag.
It authenticates with the broker's organization UUID and hexadecimal API key,
then uploads a complete OpenVAS SARIF report using the documented `upload-file`
input. The original XML and summary JSON remain in the workflow artifact.
Report retrieval explicitly disables pagination. Invalid or nonfinite severity
values fail qualification; partial scans cannot be uploaded. A complete report
containing vulnerabilities is still uploaded when the vulnerability gate fails.

The SuperZip workflow fails closed until `GREENBONE_SECRET_PROVIDER_URL` points
to a broker that validates GitHub OIDC claims and returns the authorized
Greenbone and Vulnetix settings. Do not add hard-coded organization IDs,
tokens, URLs, scan targets, or credentials to the repository.
Manual workflow-dispatch target text is sent to that broker only as a target
request; the checked-in workflow must use the broker-returned
`greenbone_target` as the effective OpenVAS target.

Recheck the annotated tag's resolved commit, supported inputs, authentication
and accepted report formats before rotating the action. Keep one authenticated
upload invocation; a default informational invocation does not verify access.
