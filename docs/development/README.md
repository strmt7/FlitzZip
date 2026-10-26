# Developer Guide

Start with [CONTRIBUTING.md](../../CONTRIBUTING.md), then load the guide for the
affected component. End-user documentation is indexed [one level up](../README.md).

| Work | Canonical guide |
| --- | --- |
| Set up, build and run | [Building FlitzZip](building.md), [ROCm configuration](../rocm-toolchain.md) |
| Select and run checks | [Targeted verification](../targeted-verification.md), [test investigation](../debugging-strategy.md) |
| Design tests and review gaps | [Coverage acceptance and meaningful tests](../code-coverage.md), [HIP device measurement](hip-device-coverage.md) |
| Change archive behavior | [Format support](../archive-format-support.md), [native specification](../native-flzip-format.md), [stream contracts](../compatibility-stream-contracts.md) |
| Change GUI behavior | [Design](../design.md), [product behavior](../product-behavior-audit.md) |
| Refactor or learn from a defect | [Refactoring governance](../refactoring-governance.md), [engineering learning loop](../engineering-learning-loop.md) |
| Measure performance | [Benchmark index](../benchmarks/README.md), [comparison methodology](../comparative-benchmark-methodology.md) |
| Review source and dependencies | [Security scanning](../security-code-scanning.md), [source remediation](../security-source-remediation-redesign.md) |
| Qualify a release | [Release process](../release.md), [candidate scope](../beta-readiness.md), [implementation plan](../../IMPLEMENTATION_PLAN.md) |
| Use development research tools | [Crawl4AI](../crawl4ai.md), [context and code search](../development-context.md), [MCP](../../mcp/README.md) |
| Check provenance and obligations | [Third-party software](../third-party.md), [benchmark permissions](../benchmark-permissions.md) |

AI agents follow [AGENTS.md](../../AGENTS.md) and its scoped reading map. The
[operating guide](../agent-operating-guide.md) owns repository rules; domain
guides own the procedures linked above. Keep each command or requirement in
its canonical guide and link to it from entry points.

Current guidance, specifications and maintained benchmark evidence belong in
the published tree. Chronological progress reports, abandoned experiments and
raw host logs belong in ignored local artifacts; Git retains earlier published
revisions. Active security review evidence, license notices and regression
fixtures remain available to their consumers.
