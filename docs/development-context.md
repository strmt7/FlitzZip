# Development Context And Reusable Evidence

This is a navigation and tooling guide, not another operating policy. Start with
[AGENTS.md](../AGENTS.md) and its required-reading map. The
[operating guide](agent-operating-guide.md) remains normative. Keep public
documentation, source contracts and commands in normal, precise language.

## Small Index, Exact Sources

| Question | Open next |
| --- | --- |
| Current development and unresolved work | [Implementation plan](../IMPLEMENTATION_PLAN.md), [beta readiness](beta-readiness.md), then exact current source and GitHub checks |
| Format capabilities and wire contracts | [format support](archive-format-support.md), [native format](native-suzip-format.md) |
| GPU admission, codecs and timing | [GPU research](gpu-accelerated-ui-and-codec-research.md), [performance validation](performance-block-size-validation.md), exact `src/gpu` and `src/core` sources |
| Comparison eligibility and results | [comparison methodology](comparative-benchmark-methodology.md), [benchmark index](compression-level-and-benchmark-suite.md) |
| Validation and hosted acceptance | [Targeted verification](targeted-verification.md), selected workflows for the exact pushed commit |
| Repeated mistakes and scanner evidence | [learning loop](engineering-learning-loop.md), [security scanning](security-code-scanning.md) |
| Native GUI design and diagnosis | [design](design.md), [debugging strategy](debugging-strategy.md) |

This index routes to sources rather than copying their current SHAs, counts,
versions or policy. Historical measurements remain in Git and local evidence;
they do not establish current status. Resolve current Git, host, scanner and
workflow values when needed.

## Mandatory Skills And Actual Use

At a new context boundary, deliver the current mandatory skill files:

```powershell
py -3 tools/agent_context.py startup
```

The output contains both complete skill instructions and their source hashes,
plus the authoritative Crawl4AI research directive extracted from AGENTS.md and
the portable launcher's source identity. Startup and its offline mutation tests
fail if that research directive or launcher is removed. This delivers the
existing policy without duplicating it or installing the crawler during startup.
Missing files or an absent mandatory declaration fail explicitly. Read
AGENTS.md and the relevant domain sections as well; startup does not claim they
were reviewed. Within an unchanged context, do not reload the same skills at
every checkpoint. Re-read changed instructions and newly applicable domains.

Caveman applies to internal AI communication. Its first improvement is reducing
unnecessary input. Do not compress source, commands, safety conditions, evidence
or user-facing text. No deterministic script can prove a model followed prose
instructions; startup verifies delivery, and review checks actual behavior.

CocoIndex is the first route for broad conceptual code navigation:

```powershell
py -3 tools/cocoindex_agent_search.py index
py -3 tools/cocoindex_agent_search.py search --limit 5 "bounded GPU resource ownership"
```

Install only if the pinned tool is missing, as its skill describes. Refresh only
when code bytes or index configuration changed. Search refuses stale inputs
both before and after the package call. Successful searches atomically write a
per-checkout `.usage.json` receipt beside the external active-index marker.
It records source/config hashes, pinned version, UTC completion, query hash and
distinct-file hit count; it does not store the query text. Failed/stale searches
do not advance it. An old receipt remains historical evidence after sources
change. Indexing or installing alone never counts as successful search use.

Exact symbols, strings and known small scopes still use `rg` first. Confirm
semantic candidates with exact reads. Scores rank hints, not factual confidence.
Do not install another memory service, model or database to use this guide.

## Bounded Reads And Context Lifetime

Use heading metadata before opening a long document:

```powershell
py -3 tools/agent_context.py outline docs/agent-operating-guide.md
py -3 tools/agent_context.py read docs/agent-operating-guide.md --section "Git Workflow"
py -3 tools/agent_context.py read tools/cocoindex_agent_search.py --start 1 --end 40
```

The reader returns exact numbered lines and a full-file SHA-256. ATX headings
inside fenced examples are ignored; duplicate/missing headings are errors.
Descendants belong to their parent section. Choose line bounds for files with
other heading conventions. The default content budget is 8,000 characters;
`--max-chars` accepts 1-32,000. The budget measures emitted source characters,
not tokens or JSON metadata. Never describe it as a tokenizer or billing saving.

Oversized windows return `partial` plus the exact unread line range. Continue
from that range until required reading is complete. Long individual lines are
not silently shortened; increase the explicit budget or inspect them directly.
Only fully emitted windows can enter a read cache:

```powershell
py -3 tools/agent_context.py read docs/agent-operating-guide.md --section "Git Workflow" --session allocation-round-one
```

Repeat reads of the same range and bytes within that explicit session return a
short reference. Changed bytes, another range, another session, or `--force`
return source again. A session means one retained model context, not a chat ID.
After compaction, restart, handoff or uncertain retention, use a new session or
`--force`; a filesystem cache cannot prove the model still remembers a read.
Histories are bounded to 200 reads. No cache substitutes for tests or scanners.

## Explicit Notes With Freshness Checks

Write a short synthesis file under ignored `out/` after a useful investigation.
Record its kind and all source/evidence dependencies explicitly:

```powershell
py -3 tools/agent_context.py remember allocation-rejection-round-one --summary-file out/allocation-note.md --kind decision --source src/gpu/hip_allocation_policy.hpp --source out/allocation-evidence.json
py -3 tools/agent_context.py recall allocation-rejection-round-one
```

The examples require actual caller-created evidence files. Notes admit at most
4,000 summary characters and 32 distinct evidence files. They expire after 24
hours by default; `--expires-hours` accepts 1-168. Creation refuses an existing
key and preserves its bytes. Use a new versioned key for a revision. Publication
is atomic; no transcript capture or global user-memory mutation occurs.

Recall checks declared source hashes and expiry. Changed, missing, inaccessible
or expired evidence withholds the summary and returns exit 3 with reasons.
Malformed state fails rather than looking current. Unchanged sources do not
prove truth, complete dependencies, test acceptance or an unchanged external
service. Refresh volatile values live. Hypotheses remain hypotheses. The tool
always reports that it has not established acceptance.

State stays in ignored `out/agent-context/`. It is disposable, bounded, local and
inspectable JSON. Source reads reject traversal, secret-like filenames,
symlinks/reparse points, binary content and inputs above 1 MiB. Deleting local
state loses convenience, not repository evidence. Promote durable lessons into
existing reviewed documents/tests instead of appending another giant wiki.

## Evidence And Research Basis

[Karpathy's April 2026 idea](https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f)
motivates reusable synthesis separate from underlying sources and a small
navigation index. Community proposals are leads, not established benchmarks.
This implementation adopts explicit provenance and bounded retrieval while
retaining the repository's existing policy, documents and single-agent rule.

[GitHub's January 2026 memory engineering report](https://github.blog/ai-and-ml/github-copilot/building-an-agentic-memory-system-for-github-copilot/)
supports verifying cited source locations at use time. Its reported gains are
specific to GitHub's experiments, not measurements of SuperZip.
[GitHub's June 2026 context report](https://github.blog/ai-and-ml/github-copilot/getting-more-from-each-token-how-copilot-improves-context-handling-and-model-routing/)
describes deferred tool loading and recurring-prefix reuse. Repository tooling
can reduce emitted context; provider caching and model routing belong to the
host harness and are not claimed as features of this script.
[Anthropic's context-engineering guidance](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)
supports selective retrieval and structured notes, while cautioning against
losing important details during compaction. No delegation recommendation
overrides this repository's one-agent boundary.

Research does not support a blanket promise that more agent documentation helps.
The [September 29 revision of an AGENTS.md evaluation](https://arxiv.org/abs/2602.11988v3)
reports higher inference cost without general task-success improvement, whereas
[a separate 2026 efficiency study](https://arxiv.org/abs/2601.20404v2)
reports lower runtime/output tokens in its smaller repository sample. Their
settings and outcomes differ. Retain required rules, avoid generated repository
tours in startup context, and evaluate this helper on actual tasks before
claiming end-to-end gains.

Measure output size separately from latency and actual token use. Tests exercise
real source mutation, budget exhaustion, context changes, missing evidence,
expiry, collisions and path containment. Hosted checks exercise those same
contracts. None establishes a universal percentage saving or overall development
speedup; measurements belong in the fresh audit when collected.
