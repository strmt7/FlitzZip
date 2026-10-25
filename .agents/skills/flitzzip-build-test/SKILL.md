---
name: flitzzip-build-test
description: Build and qualify FlitzZip changes, diagnose CI failures and verify packages using the pinned Windows and AMD HIP toolchain.
---

# FlitzZip Build and Test

Use `AGENTS.md` and [targeted verification](../../../docs/targeted-verification.md)
as the command-selection contract. The operating guide's
[Build And Test Commands](../../../docs/agent-operating-guide.md#build-and-test-commands)
and [Git Workflow](../../../docs/agent-operating-guide.md#git-workflow) own resource
admission, tool provisioning, checkpoint intent and post-push acceptance.

## Procedure

1. Inspect `git status --short` and select the complete intended change range.
   Ask `tools/verification_plan.ps1` for required checks before running them.
   Use an intermediate checkpoint while further development is planned.
2. Inspect selected command roles. Tool contracts must not silently rebuild
   native products; native changes retain their actual native consumers.
   Classify unknown paths before execution.
3. Provision missing tools through existing pinned launchers. Follow
   [ROCm toolchain configuration](../../../docs/rocm-toolchain.md) for existing
   or custom SDK installations. Preserve unrelated host work and build trees.
   Verify the selected interpreter against each lock's supported versions;
   `py -3` may select a newer unsupported runtime. Use a version selector or
   process-scoped launcher selection, never a global downgrade or relaxed lock.
4. Run `tools/verify_changes.ps1` with the planned scope. Diagnose failures
   before expanding coverage. Reuse checks only while relevant source, recipe,
   tool and output identities still match.
   For an explicitly selected development test batch, the canonical native
   executor's `-ContinueAfterAssertionFailure` retains complete independent
   failures for grouped diagnosis without retries or a passing summary. Keep its
   owning process deadline, exact-name checks and backend admission intact.
   Default qualification still stops on its first failure.
5. Review the final diff and commit intentional source, documentation and
   configuration. Follow the operating guide's exact-commit workflow procedure;
   final acceptance covers the accumulated change range and required audit.

For a failing check, distinguish configure, compile, link, execution, collection
and upload failures before changing code or rebuilding. Preserve the first
failure. Use `flitzzip-test-investigation` when the cause is unclear. A retry must
test a changed hypothesis; an eventual pass does not erase an intermittent failure.

For build-time improvements, measure the expensive stage first. Review generated
outputs and their dependency edges before adding caches, PCH reuse or parallel
work. Validate an unchanged incremental build and the rebuild caused by changing
a real input. Caches accelerate work; they do not replace source/output admission
or the traced builds required by static analysis.

## Specialized consumers

| Changed behavior | Canonical qualification |
| --- | --- |
| Standard-format writing | Independent interoperability smoke selected by the verifier |
| Registry, detection or public adapter contract | Registry-driven format matrix |
| GUI state, input, settings or rendering | GUI Rules, announced all-page smoke and review of every generated capture |
| Codec performance or GPU work | `flitzzip-performance`; the selected mode's RAM-only benchmark and real HIP telemetry |
| Parser, loader, subprocess or release trust boundary | `flitzzip-security` and affected production consumers |
| Portable or MSI package | [Release qualification](../../../docs/release.md), exact artifact identities and installer lifecycle |

Product qualification uses HIP-enabled binaries. CPU-only builds serve the
hosted/static-analysis roles documented in the guide. Do not build or package
while the built GUI holds its files. Candidate qualification and permission to
publish are separate decisions defined by the release guide.
