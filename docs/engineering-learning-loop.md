# Engineering Learning Loop

SuperZip records concrete engineering mistakes as enforceable repository
invariants. The goal is not more process; it is to convert verified failures
into small checks that prevent the same regression from reaching GitHub Actions,
the Security tab, released artifacts, or the product UI again.

## Current Guardrails

- Workflow `run` blocks must not interpolate `${{ github.* }}` directly.
  `tools\verify_change_hygiene.ps1` and `tools\security_scan.ps1` require
  environment-variable indirection so Semgrep-style script-injection findings
  are caught before push.
- GitHub Actions must not use `Install-PackageProvider -Name NuGet` as a CI
  bootstrap. The lint workflow installs PSScriptAnalyzer from a pinned,
  hash-verified PowerShell Gallery package instead of depending on mutable
  runner PackageManagement state.
- The README license badge is static for the current AGPL-3.0 license. It must
  not depend on the shields GitHub license endpoint, which can fail from
  upstream token-pool exhaustion even when the repository license is valid.
- The README and application logo must derive from
  `resources\brand\superzip-logo.svg`. `tools\verify_brand_assets.ps1` checks
  that the canonical mark exists, generates the Win32 geometry from it, verifies
  the ICO, rejects SVG viewBox clipping, and forbids AI edits to the mark path
  geometry, stroke style, layer count, and source-of-truth metadata. Agents may
  move or resize the mark for layout and may edit surrounding tagline copy.
- Workflow waiting must use `tools\wait_relevant_workflows.ps1` and the plan's
  `workflowWaitPolicy`. The waiter resolves local refs to full commit SHAs,
  performs a GitHub CLI preflight, queries the Actions API by `head_sha`, and
  fails on API errors instead of polling misleading "missing workflow" states.
  Agents must not wait on completed Greenbone/OpenVAS integration runs,
  unauthenticated GitHub CLI calls, wrong commit refs, unreliable commit-filter
  wrapper results, or unrelated long-running workflows.
- CodeQL C++ must use a real manual Windows build database. Build-free C/C++
  analysis produced parser-artifact Security tab alerts for Win32/GDI+, HIP,
  and vendored C code; `tools\verify_change_hygiene.ps1` and
  `tools\security_scan.ps1` reject restoring `build-mode: none`.
- Changed-file lint must validate the GitHub push base before using it. Amended
  or force-with-lease pushes can reference a replaced sibling commit; the lint
  workflow falls back to `HEAD~1` only after proving the event base is not
  available in the full-history checkout.
- Build/package commands must not run in parallel with GUI smoke tests. The
  build script checks for a running build-output `SuperZip.exe` and fails with a
  direct close-the-app message instead of surfacing a linker file-lock error.
- Release replacement is exceptional. The release workflow requires
  `replace_existing=true` plus `replacement_acknowledgement` set exactly to
  `replace <version>`, and both `tools\verify_change_hygiene.ps1` and
  `tools\security_scan.ps1` reject release workflow/action edits that remove
  that safeguard.

## Adding A New Lesson

When a failure exposes a repeatable class of mistake, add the narrowest useful
guard in this order:

1. Fix the root cause in product code, workflow code, or documentation.
2. Add a local invariant to `tools\verify_change_hygiene.ps1` when the issue can
   be detected from changed files.
3. Add a repo-wide invariant to `tools\security_scan.ps1` when stale or
   unrelated files can reintroduce the issue.
4. Add or update selector coverage in `tools\test_verification_selector.ps1`
   only when routing behavior changes.
5. Document the lesson here in one short bullet with the concrete tool that
   enforces it.

Do not add broad exclusions, scanner suppressions, mandatory heavyweight gates,
or product-direction changes as a lesson. A learning guard is valid only when it
prevents a known failure class without hiding findings or slowing unrelated
development paths.
