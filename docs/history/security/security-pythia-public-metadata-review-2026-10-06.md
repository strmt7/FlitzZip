# Pythia Public Metadata Review, 6 October 2026

Status: individually approved by the maintainer on 6 October 2026 and applied
to the exact metadata ledger. The authorization names the eight reports and
complete proposal SHA-256 below; it admits no source or test-code finding.
This review covers eight exact `DS173237` reports in two scanned acquisition
and permission manifests. Source and test code remain outside admission.
The original raw SARIF and every unmatched finding remain retained.

## Exact Scope

The retained raw report is
`out/scanner-preflight/dcb0344769ce457785c49ca51c9cccdc/devskim.sarif`,
SHA-256 `9716451b87122ce87435d1fb8930c973723ebb14c8a44de04db955b09de289f7`.
The scanner is pinned DevSkim 1.0.100, executable SHA-256
`46bccb21bcfd5c833bb9cc1f4572721ef724ef24962a1d6b01699afbb557abe9`.
Gitleaks returned zero findings. The separate test-address report is not
eligible for this proposal; its reserved documentation-address repair must
pass analysis of the changed source before publication.

| Scanned path and line | Exact public value and structured role | Evidence |
| --- | --- | --- |
| Corpus manifest:5 | `f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb`; original Canterbury archive SHA-256 | Same publisher artifact and complete corpus inventory as the previously approved checksum review. The metadata file has changed, so its old binding is not reused. |
| Corpus manifest:28 | `94f7c35d5e9f2e9bac8ca839329f505b4d007d5d`; Pythia14M immutable artifact revision | The official repository API attributes the complete model artifact to this public commit; the descriptor URL binds the same revision and original filename. |
| Corpus manifest:32 | `116a02532db461f91386a5b20f942ff2c8d4de7341e21b55caafc3d7b25f49a1`; complete model artifact SHA-256 | Official repository API LFS object identity and size, 28,143,920 bytes. Download hashing must agree before GPU work. |
| Permission manifest:9 | `edbee35370e14030e4c785cf88200f42dc651c1eb4217c1e3963c38a12f099b0`; official `7za.exe` identity | Same independently reviewed comparator binary as the earlier exact checksum review. |
| Permission manifest:10 | `876077825e49f5a39fb472532080b91a41fec508c714eab8a51af5a51183eba0`; official `7za.dll` identity | Same independently reviewed companion binary. |
| Permission manifest:11 | `6509b5d4895103250405ab516421b4068e78b67cabc0c6eef774df31572d9fad`; official `7zxa.dll` identity | Same independently reviewed companion binary. |
| Permission manifest:23 | `8076aae03feac7c66b319579e82172eed168deed2a3f25e5e2d3c60f55e84111`; official `zstd.exe` identity | Same independently reviewed Zstandard comparator binary. |
| Permission manifest:122 | `94f7c35d5e9f2e9bac8ca839329f505b4d007d5d`; sole reviewed Pythia14M version | Public artifact commit binds the permitted local inert-byte benchmark scope and official EleutherAI/Hugging Face evidence. |

The [previous checksum review](security-public-checksum-review-2026-10-05.md)
preserves the unchanged comparator and Canterbury producer evidence. The new
model identity comes from the [official artifact API](https://huggingface.co/api/models/EleutherAI/pythia-14m/tree/main?recursive=false&expand=true).
[EleutherAI's license statement](https://github.com/EleutherAI/pythia#license)
applies Apache-2.0 to Pythia models and copyrightable artifacts. This is public
content/provenance metadata, not an authentication credential. Local byte
measurement permission does not establish universal model effectiveness or
authorize repository payload redistribution.

## Complete Binding And Proposed Admission

The complete newline-normalized corpus manifest SHA-256 is
`cde570c28a2365c4c783446c10695c7eac5dd8640cad5228d223c500d0d24714`.
The complete permission manifest SHA-256 is
`f2df82c0131161b787e0245a3c6d4d240a57b8ae83e154b20df4b4a0407f702d`.
The approved proposed CSV is retained at
`out/neutron-pythia-metadata-review-proposal-20261006.csv`; its complete SHA-256 is
`8f6906b299e0cbd73a20f9dadf50ab29389458d3dcfaf89b04e18d13f81801d3`.
It renews only the five existing checksum bindings for the two changed files
and adds the three exact model identity reports. Every other record is
unchanged. No broad path exclusion or source finding is proposed.

The typed commit consumer checks the complete reviewed file hash, exact
highlighted value and original artifact role. The acquisition role requires
the official immutable EleutherAI URL and safetensors filename. The permission
role requires that sole version and the official evidence URLs. Negative
controls reject changed source, missing roles, wrong publisher, shifted match,
source paths and unrelated findings. Recognition of these roles does not
authorize any additional disposition or establish hosted acceptance.

Individual approval is required by the
[canonical triage contract](../../security-code-scanning.md#finding-triage): the ledger
records "only individually approved public integrity matches". Approval must
cover these exact eight reports and complete bindings. Changed files, values,
roles or additional reports require fresh investigation. Hosted application
requires exact analyzed Git-blob and location matching with its individual
audit record; a local admission is not final hosted security acceptance.
