# Research Dependency Update Review, 5 October 2026

The [Dependabot pull request](https://github.com/strmt7/SuperZip/pull/59) at
`451143a2f30581da5bc4db8fff4900e52b78e018` proposed six independently selected
updates to the crawler lock. All four platform installation jobs rejected its
dependency graph. This proves a resolver conflict, not that every newer package
fails at runtime. The proposal was closed without merging after the review
described below. The one-time
runtime experiment below distinguishes declared restrictions from API failures.

Current publisher metadata and the actual consuming distributions establish:

| Proposed package | Proposed version | Current consumer requirement |
| --- | --- | --- |
| multidict | 7.0.0 | aiohttp 3.14.3 requires `>=4.5,<7.0` |
| pydantic-core | 2.49.0 | pydantic 2.13.5 requires exactly `2.46.5` |
| pyee | 14.0.0 | Playwright 1.63.0 requires `>=13,<14` |
| huggingface-hub | 2.0.0 | tokenizers 0.23.2 requires `>=0.16.4,<2.0` |
| snowballstemmer | 3.1.1 | Crawl4AI 0.9.4 requires `~=2.2` |
| xxhash | 4.0.1 | Crawl4AI 0.9.4 requires `~=3.4` |

The [latest published aiohttp](https://pypi.org/project/aiohttp/) is already
3.14.3. Upgrading it cannot currently admit multidict 7. Likewise, upgrading
the available consumer releases cannot make these proposed updates compatible.
Keep complete normal resolution, hashes, `pip check`, behavioral consumer
tests and the platform matrix. Do not force children with `--no-deps`, edit
installed dependency metadata, or expand upstream version bounds without a
separately justified and validated downstream revision. Security advisories
still require source review and repair; compatibility is not permission to
ignore a vulnerable dependency.

The current main commit `ef3ff5c223e7033355036148467ce36d1475f693` passes the
crawler installation/model API matrix on Windows, Linux and macOS with Python
3.13, and Linux with Python 3.14. Its source-built NLTK repairs the six affected
model APIs. Dependabot alert 23 remains open: the advisory has no patched
upstream release, and GitHub continues to classify the downstream local
version under the vulnerable upstream version. Do not claim alert closure
from the package version or an empty OSV report.

The PR's additional component-contract failure occurs only on its initial
branch push. That event had an all-zero previous commit; the workflow wrongly
treated every inherited file as changed, reaching unrelated unclassified
root files. The corrected production comparison uses the merge base with the
default branch for an initial feature-branch push. Initial default-branch
publication retains complete tracked-file coverage. Missing ancestry fails
closed. Real Git fixtures verify inherited-file rejection, actual changed
path retention and invalid/missing-reference refusal. No selected component
contract or product test is removed.

Evidence retained under ignored `out/`: `pr59-dependency.diff`,
`pr59-crawler-failure.log`, `pr59-component-failure.log`,
`pr59-current-pypi-metadata.json`, and `ci-initial-push-contracts.log`.

The PR and advisory were refreshed after commit
`e56becc7db74453c2ea34b83fa96464dbf156020`. The PR still has the exact head
above and remains open and unmerged. The advisory still declares
`first_patched_version: null` and affects upstream versions `<=3.10.3`.
The new portable crawler revision retains the admitted NLTK source repair;
its actual installed model/API regressions pass again. Neither a package
version change nor the incompatible PR can close that database alert honestly.

## One-Time Runtime Exception

The maintainer explicitly authorized testing newer packages beyond declared
consumer constraints for this round only. This authorization does not change
the installer, agent rules, update policy, normal resolution or admission gates.
The qualified environment and its installation receipt remained unchanged.

On Windows x64 with CPython 3.13, disposable import overlays installed the
official, non-yanked wheels after checking their publisher SHA-256 identities.
Only this isolated experiment omitted dependency resolution. A passing
unchanged-graph control preceded the assessed overrides. Each passing individual
override also passed the six actual crawler download regressions and nine NLTK
model-artifact/API regressions. No external model weights were downloaded.

| New package tested | Version | Actual consumer evidence |
| --- | --- | --- |
| multidict | 7.0.0 | aiohttp client/server: repeated query values, duplicate headers and exact body; crawler HTTP regressions passed |
| pydantic-core | 2.49.0 | Rejected by Pydantic 2.13.5's runtime version check; further isolated API diagnostic failed |
| pyee | 14.0.0 | Async events and a real headless Playwright Chromium page, click and event lifecycle passed |
| huggingface-hub | 2.1.1 | Actual Hub HEAD/GET download from an owned loopback fixture and Tokenizers byte-exact readback passed |
| snowballstemmer | 3.1.1 | English stemming and Crawl4AI's actual BM25 content selection matched the control |
| xxhash | 4.0.1 | Crawl4AI's actual content hashes and incremental hashing matched the control |

The Hub version is newer than the PR's proposed 2.0.0; this tests the latest
publisher release observed during the exception, not that exact PR wheel.
The other five versions match the PR. These are focused Windows results,
not complete upstream suites, fresh normal installations or cross-platform
qualification. They do not authorize declaring all five integrations compatible.

For the core diagnostic, a disposable copy of Pydantic changed only its expected
core-version constant from 2.46.5 to 2.49.0. The installed package and production
guard were untouched. Even beyond that version check, ordinary
`BaseModel.model_json_schema()` failed:

```text
TypeError: No method for generating JsonSchema for core_schema.type='fraction'
(expected: GenerateJsonSchema.fraction_schema)
```

This is a demonstrated API incompatibility, not merely stale dependency
metadata. The combined latest graph consequently fails. The earlier basic
validation and JSON serialization steps passed in this diagnostic; they were
insufficient to establish compatibility. Do not drop schema coverage or ship
the experimental guard change to make the grouped update pass.

The [released Pydantic schema generator](https://github.com/pydantic/pydantic/blob/v2.13.5/pydantic/json_schema.py)
lacks that handler. The [core 2.49.0 publisher source distribution](https://pypi.org/project/pydantic-core/2.49.0/)
defines the new `fraction` schema type and its constructor. Its authenticated
`python/pydantic_core/core_schema.py` member has SHA-256
`c4bc7cf4e48da8508a48bfaaa260e4c3f348b543668e590dad6f6ee0f368ad08`.
The parent needs a matching implementation; widening a requirement or changing
the expected-version constant alone does not supply it. Crawl4AI's installed
HTTP strategy read the primary publisher pages with robots checks; no browser
was started alongside the GPU benchmark.

The control also exposed an upstream serialization limitation:
`CrawlResult.model_dump_json()` without markdown raises
`PydanticSerializationError` on the unchanged qualified graph. The actual
`model_dump()` consumer with a populated markdown result passed. The broader
method limitation remains an observation; it was not repaired or attributed to
these updates.

Bounded diagnostics and exact wheel identities are retained under ignored
`out/pr59-runtime-exception-20261005.json`. Initial experiment-fixture failures
are retained separately and are not package-incompatibility evidence. Temporary
overlays and the diagnostic source copy were cleaned up. No permanent workflow,
production override or recurring requirement-bypass procedure was added.

## Review Decision, 6 October

The exact reviewed PR head remained
`451143a2f30581da5bc4db8fff4900e52b78e018`. Its incompatible grouped proposal
was closed without merging on 5 October at 22:45 UTC (6 October locally).
The qualified dependency graph and ordinary update policy remain intact.
GitHub's `MERGEABLE` field describes Git conflict status; it does not resolve
the documented package/API incompatibility. The retained diff, experiments
and review preserve the proposed updates for later compatible consumer releases.
Closing this PR does not close Dependabot alert 23 or repair an advisory database.
