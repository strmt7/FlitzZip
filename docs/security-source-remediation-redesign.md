# Source Remediation Requirements

Demonstrated defects require production repairs and fresh verification. An
individually proven false positive may instead use the maintainer-authorized
contract below. Historical approvals establish reviewed identity; they do not
prove memory safety or activate a current disposition. Completed details are retained in
[the remediation record](https://github.com/strmt7/SuperZip/blob/f27790439954ea9fba4e306bf896e04da350546b/docs/history/security/source-remediation-implementation.md).

## Repair And Closure

Trace the actual allocation, initialized region, element units, ownership and
consumer lifetime before rewriting. Inspect sibling versions and callers. Keep
independent malformed-input, fault-injection and readback controls. Follow the
[engineering learning loop](engineering-learning-loop.md) for repair sequencing.

| Report group | Required engineering boundary | Required verification |
| --- | --- | --- |
| Dictionary/hash and block-output pointers | Complete allocation geometry, initialized input and independently bounded output | Direct selection/finalizer and guarded production compression tests, fresh flow analysis |
| Packed FSE/Huffman table offsets | Typed table regions, element counts, alignment and descriptor capacity | Representation and malformed-table controls, decoder readback and sanitizer tests |
| Optimizer task pointers | An explicit owner covering every submitted worker until join, including failure cleanup | Serial and worker-pool consumers, allocation-failure and lifetime tests |
| Legacy dictionary/output/literal references | Lifetime and initialized extents enforced at actual decoder use | All supported legacy versions, independent sequence oracle and history/backpressure tests |
| Allocation/copy API reports | Matching allocation/deallocation ownership and both readable/writable extents, with ordinary error propagation | Fault injection, truncation, overlap, aliasing/alignment and canaries |
| Algorithm progress | Explicit progress and comparison-budget state without changing the algorithm | Direct match-search and suffix-ranking consumers |
| Staged headers/default allocator | Public/static/inline inclusion and allocator contracts preserved | C and C++ first/repeated inclusion and real allocator consumers |
| Upstream quality/probe observations | Actual upstream interface or research work, with truthful provenance | Relevant compiler and direct-consumer contracts |

This table records closure requirements rather than a current backlog. Use a
fresh exact-commit post-push audit for current hosted status. Replacing a primitive
name, moving a file out of coverage, discarding an old-source reproducer or
adding whole-file guards to staged headers cannot close a row. New runtime
dependencies still require the ordinary maintainer approval.

## Acceptance And Recurrence

`scanner_preflight.py` scans frozen changed publication bytes with pinned
detectors before expensive qualification. `review_findings` keeps exact source
review matches informational and unresolved. Public checksum metadata has its
separate exact identity contract; source code cannot use that metadata path.
Retain raw reports and stable SARIF categories and paths.

`github_post_push_audit.ps1` checks the complete open inventory and findings
reproduced on the requested commit despite an earlier dismissal. It revalidates
existing public-integrity approvals and the separately authorized
[individual false-positive decisions](security-reviews/current-source-findings.md).
The latter bind repository, alert, rule, scanner version/category, exact location,
complete source, relevant callers/tests, scanner configuration and review evidence
through the [committed ledger](../.github/scanner-false-positive-reviews.csv).
Both current and analyzed executable inputs must match. Changed bytes, locations,
producers or new alerts leave a report unresolved; never renew hashes without
reviewing the changed assumptions. Historical cohorts cannot activate this path.
Raw reports remain retained and scanned. Neither a disposition nor GitHub
dismissal is evidence of a production repair. The separately authorized
[governance baseline](../.github/scanner-governance-baseline.json) accepts only
the exact identified Scorecard observations; it does not assert that their
governance controls have been implemented.
CodeQL results from an earlier commit remain active when the canonical selector
proves unchanged native/query inputs. Missing analyses, failed analysis requests,
partial coverage and changed inputs cannot establish closure. Documentation-only
pushes therefore neither rebuild native code nor hide retained source findings.
The hosted selector carries missing or unfinished CPU/HIP analysis across later
pushes by comparing the latest analysis commits with current native/query inputs.
Reuse additionally requires successful CPU/HIP jobs in the owned security
workflow; a cancelled run cannot qualify merely by uploading some analysis.
Current acceptance limits belong in [beta readiness](beta-readiness.md), not in
agent instructions or changing finding-count lists.

Plan each coherent batch once and complete the affected consumers. Reuse receipts
only for unchanged inputs and their recorded scope. Native changes require new
HIP qualification. Intermediate pushes must preserve pending or failed checks;
final acceptance covers the accumulated change range and exact pushed source.
No build, scanner or regression is retired through documentation archival.
