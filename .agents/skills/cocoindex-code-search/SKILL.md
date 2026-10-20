---
name: cocoindex-code-search
description: Route broad conceptual code searches through the pinned local index, then verify exact source.
origin: adapted from strmt7/VulnerabilityScreener .agents/skills/cocoindex-code-search/SKILL.md
---

# CocoIndex Code Search

Use CocoIndex before broad or fuzzy code navigation. Exact symbols, strings,
counts and known small scopes use `rg` directly. This is a development aid,
never scanner runtime or training data.

1. Follow the `AGENTS.md` reading map. Install only when absent with
   `<python> tools/cocoindex_agent_search.py install`; create or refresh the
   index with `index` when its inputs change. First indexing may download the
   local embedding model and take minutes.
2. Run `<python> tools/cocoindex_agent_search.py search --limit 5 "question"`.
   Stale source/configuration is refused; refresh before searching. Success
   records a per-checkout source/config/query-hash receipt. Installation and an
   index marker never prove actual use or complete review.
3. Confirm candidates with exact `rg -n` and source reads. Rankings are hints,
   not proof of correctness or completeness. For a miss, use bounded `rg -l`
   and refine once. Never edit from a semantic hit alone.
4. Use exact `--path` filters; wildcard filters are rejected because the pinned
   provider returned empty results for them on the reference Windows host.
   Otherwise route semantically, then narrow paths with `rg`.
5. Preserve and disclose exact failures. Restore the tool before dependent broad
   navigation; do not substitute `rg` for semantic search. Independent work and
   exact reads of known files/symbols may continue.

State stays in a computed external user cache; `AGENT_CODE_HOME` may specify an
absolute override. The wrapper disables telemetry and uses CPU embeddings,
preserving HIP capacity. Do not rebuild a large index without relevant changes.

Never overwrite another repository's `cocoindex-code` MCP registration or
interfere with its state. The portable CLI is independent of shared registration.
Load current skills with `tools/agent_context.py startup` at a new context
boundary. Bounded document reads supplement, never replace, semantic routing
and exact source confirmation.
