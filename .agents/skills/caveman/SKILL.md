---
name: caveman
description: Mandatory lite compression for internal AI communication; preserve exact evidence, safety, and normal public prose.
origin: adapted from ZMB-UZH/omero-docker-extended .agents/skills/caveman/SKILL.md
---

# caveman

Apply lite compression to internal AI communication in every task. Reduce input
volume first; public prose remains normal.

## Route first

- Follow the `AGENTS.md` reading map; do not load the whole skill catalog.
- Run `tools/agent_context.py startup` once per new context, then use its bounded
  `outline`, `read` and `json` operations. Follow
  [development context](../../../docs/development-context.md) for exact scopes,
  continuation notes and evidence reuse. After compaction or uncertain retention,
  use a new read session or `--force`; caches cannot prove retained knowledge.
- This skill changes internal style only: never alter tool selection,
  verification, clarification, safety or correctness to save tokens. Docs,
  comments, docstrings, commits, issues, PRs, reports and messages use normal prose.
- This local overlay needs no third-party runtime. Do not import upstream hooks,
  plugin auto-loading, `.codex` hook config, natural-language auto-activation,
  `CAVEMAN_DEFAULT_MODE`, `off`, `caveman-help`, `/compress` rewriting,
  stats/statusline scripts, `caveman-shrink`, `caveman-init`, cavecrew subagents
  or smart-installer side effects.

## Compression rules

- Preserve every negation (`not`, `never`, `no`, `only`, `except`), exact date,
  symbol, number, unit, error and uncertainty. Never abbreviate code, APIs, paths,
  URLs, commands, verification results, Typst or LaTeX.
- Follow the requested language, otherwise the user's dominant language. Do not
  damage grammar, invent abbreviations/causal arrows or add words to sound terse.
  Use heavier compression only when requested.
- Do not narrate the mode or add decorative tables/emoji. Keep full logs out of
  context unless requested or needed for diagnosis.
- Preserve required progress updates and collaboration rules. Use normal detail
  for destructive actions, security guidance, migrations, runbooks, incidents,
  ambiguity or whenever compression would obscure meaning.
