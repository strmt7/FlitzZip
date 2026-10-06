# NLTK Public Commit Finding Review, 5 October 2026

Status: the maintainer approved this exact public-commit disposition on 5 October 2026.

DevSkim 1.0.100 reports `DS173237` on the `commit` field in
`third_party/upstream/nltk/1bd574cfd4e3bca82c6b145d9271a646df410707/build.json`, line 6, columns 12â€“54. The exact highlighted value is the
public Git commit `1bd574cfd4e3bca82c6b145d9271a646df410707` in the official NLTK repository.
The LF-normalized complete source-file SHA-256 is `33441b65fd641a7b785983ae8f4830932e374086d922f7e61c522445046c32e5`.

The [official commit](https://github.com/nltk/nltk/commit/1bd574cfd4e3bca82c6b145d9271a646df410707) and
[codeload source archive](https://codeload.github.com/nltk/nltk/zip/1bd574cfd4e3bca82c6b145d9271a646df410707) identify the same upstream source.
This value selects the original archive's root and records build provenance.
It is public, reproducible Git metadata, not authentication material. The
archive digest and resulting repaired wheel are independently admitted by their
SHA-256 sidecars and complete runtime lock. Removing this identifier would make
source provenance less explicit without repairing an executable vulnerability.

The original preflight and raw SARIF are retained under
`out/scanner-preflight/762e4e397a994fd58a6b6be9d2d7ee31/` in the isolated
publication candidate. Gitleaks reported zero findings. No source-code finding
is covered by this review; the NLTK executable source bypasses are repaired by
upstream code and verified by the separate model-artifact regressions.

The proposed admission uses a typed `git_commit` value, complete source-file
hash and exact highlighted region. It also requires that the path's commit,
JSON `commit` field and official NLTK codeload URL agree. Other paths, fields,
values, rules and changed source bytes remain blocking. Legacy SHA-256 reviews
keep their existing exact admission. Raw scanner reports remain unchanged.
The typed admission ledger records this single approved public-metadata report.
It grants no source-code disposition; any hosted match still requires the exact
analyzed source, rule and location to be checked independently.
