# Benchmark Permissions And Tool Selection

Reviewed **1 October 2026**. This is an evidence-backed engineering review,
not a legal opinion or a claim that all repository licensing is settled.
Execution, publication of our own numeric measurements, redistribution of
software/data, and copying third-party artwork are separate decisions.

## Active Selection

Use **7-Zip** for the mainstream ZIP/7z application baseline, **Zstd** for
Zstandard CLI comparisons, **Bandizip Standard** as the free commercial ZIP
alternative, **PeaZip** as an additional open-source application, and
**Hyperfine** as an independent timing cross-check. These
are established alternatives, not an asserted popularity ranking. Bandizip
is license-eligible, but its application adapter and edition proof must pass
before it joins a measured series. PeaZip also needs an application-level
adapter, not a renamed 7-Zip backend invocation. Eligibility is not a
measured result. No paid subscription, evaluation period or trial is required
by this selection.

Use **lzbench only for explicitly selected LZ4 and Zstd codec context**. Its
[harness license](https://github.com/inikep/lzbench/blob/v2.4/LICENSE) does not
cover every integrated codec. The selected libraries have their own
[LZ4](https://github.com/inikep/lzbench/blob/v2.4/lz/lz4/lib/LICENSE) and
[Zstd](https://github.com/inikep/lzbench/blob/v2.4/lz/zstd/LICENSE) notices.
It cannot substitute for SuperZip application or GPU measurements.

| Candidate | Execution / original numeric results | Decision and source |
| --- | --- | --- |
| 7-Zip 26.03 | Eligible with original notices | [LGPL/BSD/component terms](https://www.7-zip.org/license.txt); no RAR algorithm recreation. |
| Zstd 1.5.7 | Eligible with original notices | [BSD option](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE); no implied endorsement. |
| Bandizip 7.46 Standard | Eligible after verifying the edition | Controlling [Korean EULA](https://kr.bandisoft.com/bandizip/eula/eula.kr.pdf), sections 1(4), 2 and 11(4); commercial Standard use is permitted. No benchmark restriction was found. No software/manual/artwork redistribution is authorized here. |
| Hyperfine 1.20.0 | Eligible, timing only | [MIT option](https://github.com/sharkdp/hyperfine/blob/v1.20.0/LICENSE-MIT); retain notices, do not bundle the tool. |
| lzbench 2.4 | Eligible, reviewed modules only | GPL harness plus separate codec terms; no all-codec preset. |
| PeaZip 11.3.0 | Eligible, official package and ZIP/7z scope | [Official terms](https://peazip.github.io/peazip-tos-privacy.html) explicitly permit free professional and commercial use; retain [LGPL and component notices](https://github.com/peazip/PeaZip/blob/sources/LICENSE). No separately installed proprietary RAR/ACE plugins. |
| NanaZip 7.0.1843.0 | Not in the active selection | [Multi-license policy](https://github.com/M2Team/NanaZip/blob/main/License.md) needs package-level review; use PeaZip instead for this round. Do not treat MIT as covering every component or its CC-BY-ND icons. |
| WinRAR 7.23 | Excluded from the free-tool plan | [EULA](https://www.rarlab.com/license.htm) has a 40-day trial and paid entitlement requirements; host entitlement is unverified. Use the eligible alternatives instead, without asking the maintainer to purchase rights. |
| WinZip | Excluded; use 7-Zip or Bandizip instead | Current [Corel General Terms](https://www.corel.com/en/eula/), License Restrictions (l), restrict benchmark execution/disclosure. An overriding agreement has not been established. |

Exclusion names remain here to explain the decision, not in active result
series. Factual research links do not copy a vendor chart, logo or screenshot
and do not imply endorsement. Do not erase historical evidence or rewrite
measured results to conceal an exclusion.

Versions above were checked against official stable release endpoints on the
review date. Before a final series, `tools/benchmark_comparators.py` must check
the installed executable against the live stable release and the reviewed
permission scope. A new release does not inherit clearance automatically.
Beta builds, paid editions and new codec modules need separate review.

## Corpora And Published Material

- **Silesia: excluded from new runs and headline publication.** The
  [author page](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) supplies
  provenance and research context, not a blanket license for all components.
  Source software, documents and medical imagery have different rights.
  Existing numeric records remain historical audit evidence; no corpus
  payload is tracked. Do not infer commercial or redistribution permission.
  Replace this corpus rather than depending on future permission requests.
- **HIGGS: eligible candidate under CC-BY-4.0.** Attribute Daniel Whiteson,
  *HIGGS* (2014), UCI Machine Learning Repository,
  [DOI 10.24432/C5V312](https://doi.org/10.24432/C5V312), and link the
  [license](https://creativecommons.org/licenses/by/4.0/). Document splits and
  transformations. The [official UCI description](https://archive.ics.uci.edu/dataset/280/higgs)
  states that these data come from Monte Carlo simulations. They are an
  authentic published numeric dataset, not empirical detector observations or
  a representative mixed-file replacement by themselves. No HIGGS payload has
  been downloaded or benchmarked in this development round.
- **Household power: eligible empirical numeric workload under CC-BY-4.0.**
  The [source and acquisition contract](benchmarks/corpora.md) records the
  official dataset, attribution, exact container/member pins and three
  disjoint complete-row excerpts. This supplies real measurements without
  treating one time series as a mixed-file workload. Payloads are not shipped.
- **Other corpora: unreviewed.** Public download access, a benchmark-tool
  license, or another reviewer's use does not license a dataset. Verify every
  constituent before use; do not copy review-site archives or media.
- Publish our own raw numeric measurements and deterministic charts. Link
  methodology sources rather than reproducing their charts, article text or
  screenshots. Generated design studies are not measurements. Asset rights
  and provenance must also be recorded before publishing a generated image.

## Enforcement And Remaining Release Work

`tools/benchmark_permissions.json` records action-specific decisions, exact
reviewed software versions, edition/module limits, sources, and conditions.
`require_permission` fails closed on an unknown subject, hold, restriction,
expired review, changed version, or unreviewed scope. The comparison runner
checks execution rights before version probes or timed product commands.
This prevents an accidental run; it is not an automatic legal clearance
service. Final publication also needs a separate result/asset review.

Comparator binaries and corpus payloads are not release assets. Current
runtime notices are packaged verbatim under `licenses/` and checked against
their source texts. That check is **not sufficient** to satisfy copyleft
source/relinking obligations. In particular, corresponding wimlib source
delivery and rebuild evidence, plus the complete runtime/tool/asset inventory,
must be resolved before release. The upstream wimlib source archive and the
adapted development-tool notice are now present; final source publication
and rebuild gates remain unverified. A root license or GitHub license classifier
does not establish rights for every file or dependency.
