# SuperZip Copilot Instructions

Follow the repository-wide instructions in `AGENTS.md`. They are the source of truth for architecture, security rules, required function documentation, git boundaries, and targeted verification.

Before choosing tests or workflow waits, run:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
```

Then run the selected local checks with `tools\verify_changes.ps1 -IncludeUntracked`. Use `-Full` when the verifier escalates, a targeted check fails, or a wider regression is suspected.

For pushed commits, follow the plan's `workflowWaitPolicy`. Use
`tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode opportunistic` only while
iterating on non-critical changes, and always run `-Mode final` before handoff,
release work, or any workflow/verifier/MCP/skill/full-escalation change.
