# Third-Party Notices

## miniz 3.1.1

SuperZip vendors miniz release `3.1.1` for standards-oriented ZIP compatibility.

- Upstream: <https://github.com/richgel999/miniz>
- Tag: `3.1.1`
- Commit: `d10b03cc73475af673df40f06e5cefd1d5f940d9`
- License: MIT, preserved at `third_party/miniz/LICENSE`

SuperZip uses miniz for two bounded purposes: standards-oriented `.zip`
compatibility and per-block deflate payloads inside native `.suzip` archives.
The `.suzip` GPU boundary remains AMD HIP-only; miniz does not provide GPU
acceleration or an alternate archive pipeline.

The production copy under `third_party/miniz/` carries narrow local hardening
patches. The unmodified upstream 3.1.1 source archive and checksum are stored
under `third_party/upstream/miniz/3.1.1/` for provenance. Do not edit the
upstream archive; production fixes belong in `third_party/miniz/` and must be
covered by tests and security scanning.

## AMD HIP SDK

SuperZip is built against AMD HIP SDK for Windows. AMD's Windows HIP deployment
guidance says the HIP runtime is supplied by the AMD GPU driver, while ISV
packages may distribute additional dynamically linked HIP SDK libraries they
use. SuperZip currently links only against the HIP runtime import library and
does not redistribute the HIP runtime DLL.

The release manifest records the runtime DLL name observed at build time so the
installer, portable package, and support diagnostics can report dependency state
clearly.

## GPU Compression Libraries

Alternative GPU-compute and compression backends are evaluated in
`docs/compression-backend-evaluation.md`. The production boundary remains AMD
HIP. hipCOMP-core is tracked as a research candidate only while its upstream
README labels it early-access technology preview and not recommended for
production workloads.
