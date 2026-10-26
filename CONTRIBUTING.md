# Contributing to FlitzZip

FlitzZip is a Windows-native C++20 application. Contributions should preserve
archive bytes, explicit format capabilities, responsive user interactions and
bounded ownership of files, threads and GPU resources.

## Report a problem

Use [GitHub Issues](https://github.com/strmt7/FlitzZip/issues) for reproducible
bugs and proposed improvements. Include the build version, Windows version,
relevant GPU/driver details, exact command or UI steps, expected result and
actual error. A small input that reproduces the problem is especially useful.
Remove private data from examples and logs. Follow [SECURITY.md](SECURITY.md)
for vulnerability reports.

## Set up development

Start with the [source build](docs/development/building.md) and
[ROCm toolchain guide](docs/rocm-toolchain.md). The scripts discover installed
Visual Studio tools and provision the pinned CMake and SDK automatically.
`-HipPath` selects an existing or custom SDK; a global `HIP_PATH` is not required.
Use a supported Python interpreter as documented by each provisioning tool.

AI agents must read [AGENTS.md](AGENTS.md) and the relevant operating-guide
sections before editing. Human contributors can use the
[documentation index](docs/README.md) to find the affected domain contracts.

## Make and verify a change

Keep each change focused on one behavior or coherent refactor. Establish the
expected public result before editing. Fix a demonstrated defect at its shared
boundary and retain a small independent regression test. Update user guidance
when a command, format capability, requirement or visible behavior changes.

Select verification from the actual change scope:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verify_changes.ps1 -IncludeUntracked -Checkpoint intermediate
```

Inspect the selected command roles. Documentation and tool contracts can run
without rebuilding the product; native, format, GUI and packaging changes keep
their required consumers. Use the accumulated commit range and a final checkpoint
before completing a contribution. See [targeted verification](docs/targeted-verification.md).

Meaningful tests exercise independent expected bytes, rejection, cancellation,
cleanup and successful reuse. Follow the [coverage policy](docs/code-coverage.md#release-acceptance).
Do not remove guards, suppress findings or pad tests to improve a percentage.

## Review expectations

- Explain the problem, resulting behavior and validation a reviewer can repeat.
- Preserve unrelated work and keep generated outputs, secrets, personal paths
  and host configuration out of commits.
- Preserve C++20/RAII ownership, explicit backend policy and documented function
  contracts. Follow [refactoring governance](docs/refactoring-governance.md).
- Keep pinned provenance and third-party licenses intact. New runtime
  dependencies require the approval defined in the operating guide.
- Use RAM-only development benchmarks and report complete archive sizes,
  independent readback and actual backend telemetry for performance claims.
- Require the selected hosted workflows and exact-commit audit to pass.

CPU-only validation is available for hosted or analysis roles through the
explicit build option. It cannot qualify required-HIP behavior or a product
release. GUI changes require the announced all-page smoke and capture review.
Packaging and publication follow the [release guide](docs/release.md), including
exact ZIP/MSI qualification and separate maintainer publication approval.
