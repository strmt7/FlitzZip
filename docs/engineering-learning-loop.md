# Engineering Learning Loop

The [operating guide](agent-operating-guide.md#engineering-quality-baseline)
contains the normative engineering requirements. This guide describes how a
confirmed defect becomes a production repair and an executable recurrence guard.

## Repair Sequence

1. Preserve the original failure, affected source identity, tool/configuration
   identity and relevant output. Keep raw logs and host details in ignored local
   artifacts; publish only evidence that is safe and needed for review.
2. Trace the failing boundary and inspect sibling callers and versions. Establish
   ownership, lifetime, capacity, geometry and error propagation before rewriting.
3. Repair the shared production mechanism. Preserve format semantics and original
   regressions; do not close findings by suppression, primitive renaming, reduced
   coverage or an unintended fallback.
4. Retain the smallest independent reproducer. Where practical, demonstrate that
   the old source fails and the repaired source passes using an external oracle,
   guarded boundary or controlled negative test.
5. Ask the change-aware verifier for affected consumers. Diagnose a failed check
   before expanding coverage. Keep deferred checks explicitly deferred; match
   source and tool identities before reusing successful evidence.
6. Verify the pushed source through the selected workflows and required hosted
   audit. A local pass, push, closed pull request or unrelated green workflow
   does not establish finding closure or release acceptance.

## Durable Knowledge

Update the existing agent rule or component contract only when the defect reveals
an enduring requirement. Instructions and skills contain reusable procedures,
not changing finding counts or chronological progress reports. Prefer extending
an existing executable guard over adding another parallel policy framework.

Completed source-specific lessons and their original guard references remain in
[the engineering record](history/development/engineering-learning-record.md).
Archiving prose does not retire any test, scanner or enforcement boundary.
Optional accelerator registration must not execute before capability admission.
Keep startup-absence regressions alongside real accelerator execution tests;
compile-only checks cannot qualify either runtime behavior. Across separately
linked CRTs, use an explicit versioned interface with ownership retained by its
allocator and no exception propagation across the boundary. Validate packaged
bytes independently from hardware availability.
Produce runtime digests from linked artifacts, not configure-time placeholders.
PowerShell analyzer lint must also parse source with the native grammar parser;
an analyzer's empty finding list does not establish syntactic validity.
Current source-remediation requirements remain in
[the remediation guide](security-source-remediation-redesign.md).
