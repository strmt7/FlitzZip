---
name: cocoindex-code-search
description: Route broad conceptual code searches through the pinned local index, then verify exact source.
origin: adapted from strmt7/VulnerabilityScreener .agents/skills/cocoindex-code-search/SKILL.md
---

# CocoIndex Code Search

For broad or fuzzy repository code navigation, use this workflow before
opening many files. Keep direct `rg` first for exact symbols, strings,
counts and small known scopes. CocoIndex is a development aid and never
part of scanner runtime or training data.

1. Use the `AGENTS.md` reading map to identify the likely domain. If the local
   index is missing, run `<python> tools/cocoindex_agent_search.py install`,
   then `<python> tools/cocoindex_agent_search.py index`.
   The first index may download a local embedding
   model and take several minutes. Installation and index data stay in a
   computed user cache, outside the live checkout; `AGENT_CODE_HOME` may
   override it with an absolute path.
2. Run `<python> tools/cocoindex_agent_search.py search --limit 5
   "conceptual question"`. Search rejects source changes until `index`
   refreshes the code-only mirror. The wrapper disables usage telemetry
   and uses CPU embeddings, preserving HIP capacity for SuperZip work.
3. Confirm each relevant candidate with `rg -n` and an exact source
   read. The first-five ranking is a hint, not evidence of completeness
   or correctness. If semantic results miss the target, use bounded
   `rg -l` and refine once. Never change code based on a semantic hit
   alone.
4. Exact `--path` file filters work. CocoIndex Code 0.2.41 returned
   empty results for wildcard filters on the reference Windows host;
   this wrapper rejects them. Use unscoped semantic routing followed by
   `rg` for path narrowing.
5. If install/index fails twice, inspect the exact error and use bounded `rg`
   so a development tool does not block a correctness fix.

Do not register over an existing `cocoindex-code` MCP server bound to
another repository. This repository's CLI workflow is independent of
that host configuration and transfers through tracked files.
