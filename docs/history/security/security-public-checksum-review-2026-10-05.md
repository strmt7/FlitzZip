# Public Checksum Finding Review, 5 October 2026

Status: the maintainer explicitly approved these ten exact checksum
dispositions on 5 October 2026. Every raw report remains retained. This record does not authorize source
findings or change scanner rules. Hosted dispositions require individual current
binding and an audit comment; this approval does not apply them automatically.

The original reviewed local preflight reported ten `DS173237` matches in integrity
metadata and three `DS162092` matches in the pending Neutron corpus controller.
The latter were outside this review and are addressed by the separate transport
redesign. That initial Gitleaks report contained zero findings;
hosted history scanning and the exact pushed-commit audit remain required.

## Evidence And Scope

The raw preflight receipt and SARIF are under
`out/scanner-preflight/1e160cc3b40a46e4a7bf6e2153739e23/`.
The [triage contract](../../security-code-scanning.md#finding-triage) requires checking
each highlighted value against its structured role and producer. The upstream
generic hexadecimal-literal rule remains enabled. An integrity hash is public
and reproducible from its input; removing it would remove a verification check.

| Scanned file | Reviewed LF-normalized source SHA-256 |
| --- | --- |
| `docs/benchmarks/corpora/neutron-public-corpora.json` | `694da3700ebd5d7c9f528806f35b12533cb844d35a6c344a39cae56ef3a93a64` |
| `tools/benchmark_permissions.json` | `69c1f0aff3c2d92f34b556616066e8de9fecf4b6545ddbed925e38f9d93e08f8` |
| `docs/licenses/development-notices.json` | `fb407a5e1077a4e54b0dcc6471f772b8d19bd33f8ee1cf65471a89e132329a76` |

## Individual Matches

| File and line | Value and role | Producer and verified consumer |
| --- | --- | --- |
| Corpus manifest:5 | `f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb`; Canterbury `archive_sha256` | SHA-256 of the original [publisher response](https://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz), previously measured in RAM. The corpus decoder requires exact archive size, digest, complete member inventory and CRCs. This is a locally measured publisher-response pin, not a separately signed upstream digest. |
| Benchmark permissions:9 | `edbee35370e14030e4c785cf88200f42dc651c1eb4217c1e3963c38a12f099b0`; 7-Zip `7za.exe` | Recomputed against the installed official 26.03 x64 comparator on 5 October; exact match. Comparator admission requires the recorded binary identity and action-specific permission. |
| Benchmark permissions:10 | `876077825e49f5a39fb472532080b91a41fec508c714eab8a51af5a51183eba0`; 7-Zip `7za.dll` | Recomputed against the official 26.03 x64 library; exact match. The same admitted comparator distribution consumes this library. |
| Benchmark permissions:11 | `6509b5d4895103250405ab516421b4068e78b67cabc0c6eef774df31572d9fad`; 7-Zip `7zxa.dll` | Recomputed against the official 26.03 x64 library; exact match. The distribution's extraction library identity remains pinned. |
| Benchmark permissions:23 | `8076aae03feac7c66b319579e82172eed168deed2a3f25e5e2d3c60f55e84111`; Zstd `zstd.exe` | Recomputed against the official 1.5.7 win64 comparator; exact match. Execution requires the pinned binary and permission record. |
| Development notices:9 | `f0abc56b6f49ab2e285bb6e6723f028abb7ebd4fe0e242bbdc2b4dded0ace8b9`; Caveman license `sha256` | Matches [the retained MIT original](../../licenses/Caveman-MIT.txt) from the exact upstream snapshot referenced by the adapted prompt. The executable inventory rejects changed notice bytes. |
| Development notices:16 | `40e82e1afe000f110e37de76229be158e1c8def078e35f855a453c068666e19d`; Caveman scope `sha256` | Matches [the retained scope declaration](../../licenses/Caveman-licensing-scope.md) from the same upstream snapshot. It preserves the distinct Engine licensing boundary. |
| Development notices:23 | `193fe32704bee15cd74f5153e569cdf830e26445e19798a55daee32da40758aa`; Crawl4AI license `sha256` | Matches [the retained 0.9.4 original](../../licenses/Crawl4AI-0.9.4.txt), including its appended attribution requirement. The inventory and attribution contract both pass. |
| Development notices:30 | `f1736a066b6f5e00cf70decad461c725272c26d844adf13796985acd8c4e3f7c`; adapted wrapper notice `sha256` | Matches [the retained VulnerabilityScreener MIT notice](../../../third_party/notices/VulnerabilityScreener-MIT.txt). The inventory also requires every adapted-tool notice to be listed. |
| Development notices:37 | `5e8c0ba20456f84943cba547175e212962117b43ad1b20c877aa983429eb256c`; CocoIndex Code license `sha256` | Matches [the retained 0.2.41 license](../../licenses/CocoIndex-Code-0.2.41.txt) from the installed pinned distribution. The inventory rejects mutation or removal. |

## Admission Boundaries

The metadata ledger records these exact values and complete source-file hashes
after the maintainer's approval. Its eligible roles now also include the exact
benchmark-permission and development-notice manifest paths. It retains complete file-hash
binding, exact highlighted-value matching, raw reports, and detection of every
other value or rule. Source and test code remain ineligible.

No approval can establish benchmark redistribution rights, release licensing
completion, GPU stability, or general absence of secrets. Changed source bytes,
changed highlighted values, failed secret scans or an unfamiliar field require
new investigation. Hosted false-positive disposition requires the specific
individual audit comment and does not substitute for a completed scan.

## Hosted Application

The security workflow for commit `51edd74b593e111aae802cee8c55437e85081e62`
completed successfully. Its DevSkim 1.0.100 report surfaced six corresponding
metadata alerts: 8296 for the Canterbury pin and 8297–8301 for the five retained
notice pins. Each exact committed source hash, highlighted value, structured
checksum role and returned location was matched to the approved record before
applying its individual false-positive disposition. The raw SARIF, alert snapshot,
binding plan and API responses remain retained under ignored `out/`.

No source-code alert was disposed. The two accepted external governance findings
remain visible. This metadata decision does not certify native code, GPU stability,
benchmark rights or the absence of other raw scanner observations.
