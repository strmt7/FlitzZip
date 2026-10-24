# Development Context And Reusable Evidence

This is a navigation and tooling guide, not another operating policy. Start with
[AGENTS.md](../AGENTS.md) and its required-reading map. The
[operating guide](agent-operating-guide.md) remains normative. Keep public
documentation, source contracts and commands in normal, precise language.

## Small Index, Exact Sources

| Question | Open next |
| --- | --- |
| Current development and unresolved work | [Implementation plan](../IMPLEMENTATION_PLAN.md), [beta readiness](beta-readiness.md), then exact current source and GitHub checks |
| Format capabilities and wire contracts | [format support](archive-format-support.md), [native format](native-flzip-format.md) |
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
py -3 tools/agent_context.py outline tools/crawl4ai_tool.py
py -3 tools/agent_context.py read docs/agent-operating-guide.md --section "Git Workflow"
py -3 tools/agent_context.py read tools/cocoindex_agent_search.py --start 1 --end 40
```

Outlines are paged: `--offset` continues from `next_offset`, and `--limit`
selects at most 100 entries. The default outline output budget is 4,000 serialized
characters. Python outlines use the syntax tree without importing or executing
source; they include qualified names, decorators and exact line spans. Other
files use Markdown headings; use exact `rg -n` symbols for other languages.
An outline routes reading and does not establish review of the listed code.

The reader returns exact numbered lines and a full-file SHA-256. ATX headings
inside fenced examples are ignored; duplicate/missing headings are errors.
Descendants belong to their parent section. Choose line bounds for files with
other heading conventions. The default content budget is 8,000 characters;
`--max-chars` accepts 1-32,000. The budget measures emitted source characters,
not tokens or JSON metadata. Never describe it as a tokenizer or billing saving.

Context access and CocoIndex mirroring share a repository-relative path policy.
It rejects secret-like names, case variants, Windows trailing-dot/space aliases,
path escapes and reparse traversal before reading or writing those paths.

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

## Report Inspection And Output Discipline

Keep full scanner, test, benchmark, research and workflow results in their existing
local artifacts. Inspect a report's shape, then select the fields needed for the
current decision:

```powershell
py -3 tools/agent_context.py json out/verification-plan.json
py -3 tools/agent_context.py json out/verification-plan.json --pointer /requiredLocalCommands
py -3 tools/agent_context.py json out/verification-plan.json --pointer /requiredLocalCommands --field id --field command
py -3 tools/agent_context.py json out/verification-plan.json --pointer /requiredLocalCommands/0
```

Use actual artifacts, not these example names unless they exist. JSON inspection
accepts UTF-8 reports up to 16 MiB, returns the complete file's SHA-256, and does
not rewrite evidence. Exact RFC 6901 pointers select members: `~1` encodes `/`,
`~0` encodes `~`, and array indexes are zero-based. Ambiguous duplicate keys,
non-finite numbers, missing members and invalid paths fail explicitly.

The default page contains at most 12 immediate members and 4,000 serialized
characters. Containers and large scalars show their type and extent with
`expanded=false`; they have not been read. Follow their pointer or `next_offset`
to continue. `--limit`, `--offset` and `--max-chars` control the page. A page that
cannot fit even one member fails instead of silently dropping it. Every result
states that the context tool has not established acceptance. The report's own
status, failures and counts must still be inspected; a successful reader exit
only establishes a successful read.

For arrays of objects, repeat `--field` to project up to 16 exact member names
per row in one call. Missing fields fail explicitly. Projected rows still carry
`expanded=false`: selected fields do not imply their other contents were read.

Apply the same discipline to all agent tool calls, including MCP and shell:

- Choose exact paths/fields before printing. Use `rg --files`, `rg -l`, `rg -c`
  or bounded `rg -n` according to the question. Do not dump whole collections
  just to find one field. Match the combined output budget to batched reads.
  Bound characters as well as rows: one JSON record or log line can exceed the
  entire context budget. In PowerShell, pass wildcard filename filters with
  `rg -g` rather than assuming shell expansion of a wildcard path.
  Decode text artifacts explicitly as UTF-8; use Python's `-X utf8` for Windows
  snippets that print Unicode rather than retrying an unchanged encoding failure.
- Keep each command's exit status and every unresolved failure visible. Retain
  complete evidence before projecting it. Never pipe away failures, call a
  truncated output complete, or repeat an expensive command to retrieve text
  already saved locally.
- For a passing check, return its result, scope, evidence location and relevant
  identity. For a failure, inspect the exact failing case and diagnostics before
  widening scope. Tool output limits affect presentation, not executed checks.
- Use native Crawl4AI captures and compact diagnoses for research. Read relevant
  extracted passages from retained captures; do not repeatedly crawl or print an
  entire site. Respect the research access and freshness rules.
- Do not modify a working MCP/provider registration or add an agent dependency,
  hidden summarizer or external model to reduce context. Existing bounded MCP
  process ownership and output limits remain mandatory.

Keep canonical rules in the operating guide, navigation in AGENTS.md, domain
procedures in their existing documents and current work in the implementation
plan. Skills route to these sources instead of copying them. Do not minify code,
strip contracts, remove tests or shorten public documentation at the expense of
clarity. Output character counts are measurable; they are not token, billing,
quality or end-to-end performance guarantees.

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

At a substantial work boundary, update one concise local continuation note:
objective and current user steering, exact working tree/commit, changed files,
completed evidence and its identities, unresolved failures, active owned process
IDs and the next concrete action. Link artifacts instead of copying logs,
transcripts or old checkpoint narratives. Separate verified facts, hypotheses
and deferred work. After compaction, refresh volatile Git/process/service state;
reuse unchanged evidence rather than restarting the whole investigation.

## Evidence And Research Basis

### Skill Maintenance And Evaluation

Keep a skill only when its trigger and decision rules improve a distinct workflow.
The [official skill model](https://developers.openai.com/plugins/concepts/skills)
loads discovery metadata before the full instructions; concise, discriminating
descriptions therefore matter even when a skill is not selected.
[Skill authoring](https://developers.openai.com/plugins/build/skills) recommends
focused workflows and supporting resources only where needed. Shared repository
policy stays in its canonical guide; skills route to it rather than copying it.

Evaluate changes on representative direct and indirect requests, a neighboring
request that should not activate the skill, and a failure or ambiguity case.
Check actual decisions, commands, rejected stale evidence and resulting artifacts,
not just the presence of prescribed words. Reuse existing contract tests for
deterministic mechanics. Instruction-only edits need structural/link checks and
serial scenario review; they do not justify new frameworks or native rebuilds.
Apply the skill to real work and retain the result locally. A self-reviewed
scenario is not an independent agent evaluation or proof of automatic activation.

[OpenAI's skill-evaluation guidance](https://developers.openai.com/blog/eval-skills)
supports behavior-based checks and explicit success criteria. Where harness token
usage or command traces are available, compare them on comparable tasks; document
size alone is not an efficiency benchmark. Model-specific [prompt guidance](https://developers.openai.com/blog/rethinking-skills-and-prompts-for-gpt-6-astra)
does not authorize removing repository safety rules, changing global settings or
spawning workers. Keep mandatory tools and the single-agent boundary intact.

### Context Evidence

[Karpathy's April 2026 idea](https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f)
motivates reusable synthesis separate from underlying sources and a small
navigation index. Community proposals are leads, not established benchmarks.
This implementation adopts explicit provenance and bounded retrieval while
retaining the repository's existing policy, documents and single-agent rule.

[GitHub's January 2026 memory engineering report](https://github.blog/ai-and-ml/github-copilot/building-an-agentic-memory-system-for-github-copilot/)
supports verifying cited source locations at use time. Its reported gains are
specific to GitHub's experiments, not measurements of FlitzZip.
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
