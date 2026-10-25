# Python Tooling Locks

The Gitleaks and DevSkim packaging manifests in this directory also pin the
exact upstream scanner artifacts used by the local preflight bootstrap. The
Gitleaks version must match the hosted container pin; DevSkim's NuGet package
must match its complete byte length and SHA-256 digest before local installation.

The `.in` files declare direct tools and advisory minimum versions. The `.txt`
files contain the complete, hash-locked dependency graph used by CI. Update the
inputs and resolve the graph together. Do not silently force a transitive version
outside its parent's declared requirements. Ordinary dependencies use pinned
official artifacts. The current Semgrep lock has one explicitly identified
packaging exception, described below; it does not authorize further overrides.
The crawler has its own [portable installation and qualification](../../docs/crawl4ai.md).

## Component Contracts

`requirements-contracts.in` declares the development-only PEP 508 parser used by
scanner dependency-graph tests. Its lock admits the same universal `packaging`
wheel as the scanner and research locks. The component workflow explicitly selects
CPython 3.13, installs this small hash-locked graph, and directs `py -3` to that
same version. Runner images and unrelated local tools are not dependency providers.

For local component execution, the verification planner invokes
`tools/run_locked_python_tests.ps1` for the scanner dependency contracts. It
selects an installed supported Python interpreter and provisions this hash-locked
graph into an ignored, isolated environment. `FLITZZIP_PYTHON` can select an
explicit supported interpreter. Direct execution of other component commands may
set process-scoped `PY_PYTHON3` after installing the same lock into that
interpreter. These dependencies do not enter product binaries or packages.

## Refresh

Use a reviewed uv installation at version 0.13.4 or newer. Earlier versions
are affected by [the bundled CA-store advisory](https://github.com/advisories/GHSA-hq85-5cpj-mr62).
Verify the selected executable's version and provenance before resolution.
The certifi advisory floor in the Linux inputs and exact crawler lock must
also remain in place when refreshing dependencies.

Resolve Linux tools for the Ubuntu 24.04 x64 / CPython 3.14 runner, and Windows
linters for the oldest supported local interpreter, CPython 3.12. Also validate
the Windows lock against CI's CPython 3.14. Select a compatible Python
interpreter before running the following PowerShell example from the repository
root. On Windows it may be `py -3`; other hosts can use their selected `python3`.
The example uses `python` for that selected interpreter.

Build the exact reviewed Semgrep wheel into a new owned directory before
resolving its local distribution version. The builder verifies both upstream
and derived artifact identities and refuses an existing output directory.

```powershell
$semgrepWheelDirectory = Join-Path (Get-Location).Path (
    'out/semgrep-lock-' + [Guid]::NewGuid().ToString('N'))
python -B -m tools.semgrep_distribution --target linux --output $semgrepWheelDirectory
if ($LASTEXITCODE -ne 0) { throw 'Semgrep artifact admission failed.' }

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
    $resolverArguments = @(
        'pip', 'compile', "$stem.in", '--output-file', "$stem.txt",
        '--python-version', $python, '--python-platform', $platform,
        '--generate-hashes', '--emit-index-url', '--emit-build-options',
        '--no-annotate', '--no-header', '--no-python-downloads')
    if ($lane -eq 'semgrep') {
        $resolverArguments += @('--find-links', $semgrepWheelDirectory)
    }
    uv @resolverArguments
    if ($LASTEXITCODE -ne 0) { throw "Resolution failed: $lane" }
}
```

For a targeted update, add `--upgrade-package <package>` to the affected
resolution. A broader `--upgrade` refresh needs compatibility review for every
changed consumer. Review the resulting diff and retain advisory floors; do not
remove parent constraints merely to make resolution succeed.

Validate installation with `pip --require-hashes --only-binary=:all:` on the
target OS, run `pip check`, and exercise each tool before publishing. Cross-OS
`pip download` checks wheel availability and hashes only when used with
`--no-deps`: pip still evaluates dependency environment markers against the
host. Use uv's explicit target platform for dependency resolution, and hosted
Linux execution for the final runtime check. Semgrep installation also needs
`--find-links` pointing to the exact admitted derived wheel, as CI does.
Never count a Windows-only marker
failure as evidence that the Linux dependency graph is broken.

## Compatibility Review

The current [Semgrep artifact manifest](semgrep-distribution.json) identifies
the exact upstream wheels, downstream distribution version and narrowly
authorized MCP and OpenTelemetry requirement changes. The builder preserves scanner source,
executables, unrelated requirements and original notices. This packaging
revision is not an official Semgrep release. Its provenance and rationale
remain in [the scanning guide](../../docs/security-code-scanning.md#scanner-dependency-remediation).

CI installs the hash-locked graph through normal dependency resolution and runs
`pip check`, the actual MCP/JWT consumer regressions, CLI/SARIF checks and the
full scanner with its tracked-path coverage audit. Keep those consumers intact.
These dependencies belong to development tooling, not the native application.

An update must preserve exact artifact provenance, resolve within the reviewed
parent requirements and pass its affected runtime consumers. Group related
dependency changes when they need coordinated validation. A local resolver or
unit-test pass is not evidence of GitHub alert closure; verify the pushed
revision's workflows and dependency inventory before claiming remediation.
