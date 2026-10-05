# Research Dependency Update Review, 5 October 2026

The [Dependabot pull request](https://github.com/strmt7/SuperZip/pull/59) at
`451143a2f30581da5bc4db8fff4900e52b78e018` proposes six independently selected
updates to the crawler lock. All four platform installation jobs reject its
dependency graph. This is a real compatibility failure, not a scanner false
positive, and the proposed lock must not be merged.

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
