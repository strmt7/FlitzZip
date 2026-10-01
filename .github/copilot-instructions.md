# SuperZip Copilot Instructions

Follow the repository-wide instructions in `AGENTS.md`. They are the source of truth for architecture, security rules, required function documentation, git boundaries, and targeted verification.

Before choosing tests or workflow waits, run:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
```

Then run the selected local checks with `tools\verify_changes.ps1 -IncludeUntracked`. Use `-Full` when the verifier escalates, a targeted check fails, or a wider regression is suspected.
The verifier selects `tools\lint.ps1 -CppMode Changed` when docs, workflows,
PowerShell, Python helpers, CMake, or C/C++ formatting surfaces changed.

Choose planner/verifier `-Checkpoint intermediate` autonomously when further
development is planned, including workflow/verifier/MCP/skill and full-escalation
changes. Sample the exact pushed SHA once with `-Mode opportunistic
-IncludeLongRunning`, record unfinished gates as pending, and continue independent
work. Final review/handoff or release requires `-Checkpoint final` and `-Mode final
-FinalCommit` over the accumulated change range. Follow the operating guide's
RAM admission and cooperative-priority policy; never change unrelated host work.
