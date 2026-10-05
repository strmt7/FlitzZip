# License And Action-Permission Audit

Reviewed on 5 October 2026. This record distinguishes retained notices, local
execution rights, publication rights and redistribution obligations. It does
not retroactively authorize an undocumented historical action or certify a
binary release. The root [AGPL-3.0 license](../LICENSE) governs SuperZip's own
work; third-party components retain their original terms.

## Native Dependencies

The existing [native notice manifest](../resources/licenses/license-notices.json)
contains the product license and complete recorded texts for all eight current
vendored dependency roots. `tools/verify_license_notices.ps1` passed on the
current checkout, including generated notice content and GUI integration.
The new `tools/license_inventory.py` checks that no additional vendored root
or adapted-tool notice can silently escape the inventory. Those checks detect
coverage and byte changes; they do not decide legal rights from a file name.

| Component | Selected terms and retained evidence | Local use and remaining conditions |
| --- | --- | --- |
| miniz 3.1.2 | [MIT](../third_party/miniz/LICENSE) | Retain original copyright and permission text; production modifications are documented in its integration notes. |
| bzip2 1.0.8 | [bzip2 license](../third_party/bzip2/LICENSE) | Preserve attribution, identify the modified production copy, and avoid implied author endorsement. |
| XZ Embedded | [upstream permissive grant](../third_party/xz_embedded/COPYING) | Applies to the selected userspace decoder files; no claim that every kernel-tree file has the same terms. |
| LZMA SDK 26.03 | [public-domain declaration](../third_party/lzma_sdk/LICENSE) | Preserve the upstream declaration and contributor origins; this is the SDK subset, not the entire 7-Zip application. |
| LZ4 1.10.0 | [BSD-2-Clause](../third_party/lz4/LICENSE) | Retain source and binary notices. |
| Zstandard 1.5.7 | [BSD-3-Clause option](../third_party/zstd/LICENSE) | The alternative GPL text is also retained; using the documented BSD option does not relicense the upstream project. |
| Lhasa 0.6.0 | [ISC](../third_party/lhasa/COPYING.md) | Preserve the complete original notice; modified-source integration remains documented. |
| wimlib 1.14.5 | [dual-license scope](../third_party/wimlib/COPYING.txt), [LGPL](../third_party/wimlib/COPYING.LGPL.txt), [GPL alternative](../third_party/wimlib/COPYING.GPLv3.txt) and [libdivsufsort](../third_party/wimlib/COPYING.libdivsufsort-lite.txt) | Windows libwim uses the LGPL option without libntfs-3g. Corresponding-source delivery and modified-library rebuild evidence remain mandatory release gates. |

No native license was replaced, paraphrased or weakened. The current release
policy already requires a complete corresponding-source offering, including
the pinned wimlib source archive, at the release location. Its
[source and rebuild contract](third-party.md#wimlib-1145) remains unresolved
until the actual release artifacts and rebuild are verified. Passing a
notice-generation test cannot close that obligation. The HIP driver/runtime
remains a host prerequisite and is not newly redistributed by this work.

## Development Tools And Adaptations

[Development notices](licenses/development-notices.json) record each retained
original's identity, origin and terms. The inventory fails if its bytes change,
its source disappears, a new adapted-tool notice is unlisted, or a vendored
dependency appears without native notice coverage.

- **Caveman:** the local prompt overlay derives from the ZMB-UZH adaptation,
  whose provenance identifies the upstream prompt snapshot at commit
  `9aa63945a349bef17206540650db48c30fafbdf2`. The exact
  [MIT license](licenses/Caveman-MIT.txt) and
  [directory scope declaration](licenses/Caveman-licensing-scope.md) are now
  retained. The prompt/skill surface is MIT; Engine-linked components have
  different terms and are not imported. The local instructions are modified
  for SuperZip and must not be represented as an unmodified upstream skill.
  This repairs the missing upstream notice without copying or installing the
  current Caveman engine, hooks, extensions or fonts.
- **CocoIndex:** installed CocoIndex Code 0.2.41 and CocoIndex 1.0.24 report
  Apache-2.0 metadata. The Code package's exact
  [license text](licenses/CocoIndex-Code-0.2.41.txt) is retained; the core's
  upstream third-party notice remains in its isolated installation. The adapted
  repository wrapper already carries the separate
  [VulnerabilityScreener MIT notice](../third_party/notices/VulnerabilityScreener-MIT.txt).
  The selected [Snowflake embedding model](https://huggingface.co/Snowflake/snowflake-arctic-embed-xs)
  publishes Apache-2.0 terms. This development cache and model are not product
  release assets.
- **Crawl4AI:** release 0.9.4 uses Apache 2.0 terms with an **additional
  attribution clause** in its [exact license](licenses/Crawl4AI-0.9.4.txt).
  The complete text is retained, and attribution appears in the research guide
  and both CLI help surfaces. Do not flatten this into an unqualified
  Apache-2.0 SPDX claim. The wrapper uses public upstream APIs. The identified
  `0.9.4+superzip.portable2` [source repair](crawl4ai-portable-download-repair-2026-10-05.md)
  retains the original source archive and full license, adds a modified-source
  notice, and grants Apache-2.0 terms explicitly to its new download helper.
  It changes the platform download opener and downstream identity; browser
  internals and dependency constraints remain unchanged.
  The base installation contains 96 locked packages; package metadata and
  installed notice files were inventoried separately under ignored local
  evidence. Composite NumPy/SciPy, regex, Pillow, certifi and tqdm terms must
  not be reduced to the crawler's license. These tools, Chromium and their
  transitive binaries are installed locally and are not bundled with SuperZip.
  A future redistribution needs its own complete dependency-notice review.
- **Build and verification tools:** existing Python, Microsoft development
  tools, ROCm SDK, CMake, WiX, linters and scanners retain their own installation
  terms. The repository's recorded maintainer authorization covers necessary
  acceptance for its pinned tooling, including the scoped WiX EULA path. It
  cannot establish another host's commercial entitlement. No new driver,
  account, paid crawler, Windows firewall exception or product runtime
  dependency was installed by this crawler integration.

The README badges identify the three development projects; they do not
assert vendor endorsement or runtime inclusion. They use the same small
Shields/GitHub-link style as the referenced repository, without copying its
artwork or application code.

## Benchmarks, Corpora And Earlier Actions

The existing [action-specific catalog](../tools/benchmark_permissions.json)
and [review](benchmark-permissions.md) remain the admission authority.
Execution, result publication and redistribution are separate decisions.
Unknown subjects, expired reviews, changed versions, editions and codec modules
are rejected by the existing runner before product execution.

| Scope | Current decision |
| --- | --- |
| Hyperfine 1.20.0 | Local execution and original measurements allowed under its selected MIT terms; binary redistribution is held. |
| Reviewed 7-Zip, Zstd, Bandizip Standard and PeaZip versions | Only the catalog's exact versions, editions and modules are admitted; commercial or optional plugins are not implicitly covered. |
| lzbench | Only the explicitly reviewed LZ4/Zstd modules are admitted; its harness license does not cover every bundled codec. |
| Household Power and HIGGS | UCI CC-BY-4.0 terms require dataset-specific attribution and accurate selection disclosure. Payloads are not shipped by the repository. |
| Canterbury | The publisher expressly supplies the fixed corpus for lossless evaluation. Local inert-byte research and original measurements are admitted with attribution; constituent redistribution remains held. |
| Govdocs1 Thread 0 | Digital Corpora's research-use and CC0 terms retain exceptions for separately copyrighted constituents. Use inert RAM bytes; no execution, rendering, payload publication or claim that every constituent is CC0. Full acquisition and benchmarking have not occurred in this round. |
| WinZip | Benchmark execution/publication remain prohibited without an overriding agreement; the current [Corel terms](https://www.corel.com/en/eula/) retain the benchmark restriction. |
| WinRAR and NanaZip | Held because the exact entitlement or package scope has not been cleared. |
| Historical Silesia measurements | Rights remain unresolved; retain the historical audit record, exclude new runs and headline comparisons, and do not claim retroactive clearance. |

The maintainer explicitly authorized repository repairs, local Neutron research,
RAM-only benchmarks, developer-tool installation, README updates and GitHub
commits. That authorizes the work on this repository; it cannot waive third-party
license, dataset or site terms. Accessible evidence does not prove permission
for **every** historical action. The documented Silesia and release obligations
remain explicit gaps instead of being silently accepted.

The crawler qualification read single public documentation pages, retained
local fingerprints and original validation metadata, and did not publish the
pages' bodies. W3C's challenge was recorded as blocked. Page access is not
permission to redistribute article text, images, datasets, model weights or
website archives. Research summaries and independently produced measurements
must preserve their source attribution and applicable quotation limits.

## Verification And Outstanding Work

Native notice generation passed; all current vendored roots are covered.
Offline negative controls reject unknown dependencies, missing or changed
notices, unlisted adaptation notices and path escapes. Crawler integration
contracts verify lock completeness, external cache ownership, unchanged upstream
configuration, exact dependency-version probes, preserved child output/failure,
bounded child lifetime and honest website success criteria.

Remaining obligations are the exact release source offering/rebuild,
historical Silesia rights, other-host tool entitlements, and a complete notice
packet before any future redistribution of developer environments or benchmark
payloads. No permission decision in this document lifts the open GPU stability
incident or qualifies the pending Neutron runtime changes.
