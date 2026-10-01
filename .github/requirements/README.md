# Python Tooling Locks

The `.in` files declare direct tools and advisory minimum versions. The `.txt`
files contain the complete, hash-locked dependency graph used by CI. Update the
inputs and resolve the graph together. Do not silently force a transitive version
outside its parent's declared requirements. The sole current downstream revision
is the reviewed Semgrep dependency-metadata patch described below. Its upstream
wheel and every other installed dependency are pinned to official PyPI artifacts.

## Refresh

The September 12, 2026 refresh used uv 0.12.13. Resolve Linux tools for the Ubuntu
24.04 x64 / CPython 3.14 runner, and Windows linters for the oldest supported
local interpreter, CPython 3.12. Also validate the Windows lock against CI's
CPython 3.14. Run from the repository root:

```powershell
py -3 tools/prepare_semgrep_wheel.py
if ($LASTEXITCODE -ne 0) { throw "Scanner packaging failed" }
foreach ($lane in 'semgrep', 'gvm-tools', 'zizmor', 'lint') {
    $platform = 'x86_64-manylinux_2_39'
    $python = '3.14'
    $suffix = 'linux'
    if ($lane -eq 'lint') {
        $platform = 'x86_64-pc-windows-msvc'
        $python = '3.12'
        $suffix = 'windows'
    }
    $stem = ".github/requirements/requirements-$lane-$suffix"
    uv pip compile "$stem.in" --output-file "$stem.txt" `
        --python-version $python --python-platform $platform `
        --generate-hashes --emit-index-url --emit-find-links --emit-build-options `
        --no-annotate --no-header --upgrade --no-python-downloads
    if ($LASTEXITCODE -ne 0) { throw "Resolution failed: $lane" }
}
```

Validate installation with `pip --require-hashes --only-binary=:all:` on the
target OS, run `pip check`, and exercise each tool before publishing. Cross-OS
`pip download` checks wheel availability and hashes only when used with
`--no-deps`: pip still evaluates dependency environment markers against the
host. Use uv's explicit target platform for dependency resolution, and hosted
Linux execution for the final runtime check. Never count a Windows-only marker
failure as evidence that the Linux dependency graph is broken.

## Compatibility Review

Semgrep 1.178.0's published `pyjwt[crypto]~=2.13.0` restriction excludes the
patched PyJWT 2.15.1. On 2026-10-01 the maintainer authorized practical
compatibility tests and production promotion when verified. The paired scans
use the same frozen registry rules and source snapshot, and exercise the
scanner's actual MCP JWT verifier as well as valid/invalid JWT controls.

`tools/prepare_semgrep_wheel.py` checks the official upstream SHA-256 and every
RECORD hash, preserves all scanner code and license bytes, and creates the
explicit local distribution `semgrep==1.178.0+superzip.1`. Only the distribution
version and the PyJWT requirement change semantically; dist-info paths and
RECORD are regenerated for that identity. ZIP storage and fixed timestamps
make its hash independent of host compression-library versions. The derived
wheel is built under ignored `out/scanner-wheels`, never committed or shipped
with SuperZip. The checked-in manifest records the original artifact and patch;
the normal lock pins the derived hash and PyJWT 2.15.1. No installed metadata
is edited and production installation does not use `--no-deps`.

CI and local Linux execution use this same preparation, wheel-only hashed
install, `pip check`, runtime test module and full scanner. See
[the scanning guide](../../docs/security-code-scanning.md#scanner-dependency-remediation)
for evidence and remaining coverage limits. The upstream constraint issue
remains open; this is a reviewed downstream revision, not an upstream release.
Recheck upstream at each dependency refresh and remove the patch once a compatible
upstream release passes the same gates. Dependabot still monitors the other
normal PyPI pins; the local Semgrep identity requires an explicit manifest/lock
refresh because it is intentionally not published on PyPI.

On 2026-09-12, Semgrep 1.177.0 requires MCP exactly 1.29.0. This is above the
patched minimum for the three MCP Dependabot advisories; upgrading to MCP 2.x
would violate Semgrep's contract. The SDK belongs to scanner tooling, not
SuperZip's native application runtime.

The same Semgrep release requires `exceptiongroup~=1.2.0`,
`jsonschema~=4.25.1`, `wcmatch~=8.3`, OpenTelemetry 1.37 / 0.58b0, and indirectly
`wrapt<2` and `importlib-metadata<8.8.0`. Consequently PRs 26, 29, 30, 31,
32, 33, and 34 cannot be applied independently. Keep these constraints until
the parent packages support newer versions; do not remove scans or override
dependency metadata to make an update appear successful.

The refreshed locks supersede the versions proposed in PRs 27, 28, 35, and
46. GitHub alert closure and PR disposition require a verified push; resolving
a local lock is not remote remediation evidence. Grouped version-update PRs
keep related actions and Python requirements reviewable as coherent sets.
