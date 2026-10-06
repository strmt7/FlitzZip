# Beta Readiness

The configured candidate is SuperZip 0.8.0 for Windows 11 x64, built with HIP.
A candidate build and its validation are separate from permission to publish.

## Product Evidence

The retained native qualification covers the main test registry, seven CTest suites,
required-HIP and CPU readers across all seven block settings, binary benchmark
input and the frontend's Neutron capability/settings gates. Local reuse requires
the successful native receipt to match current inputs, configuration and outputs.
Hosted compilation covers the six release HIP targets; it does not prove runtime
behavior on every AMD device.

The PPMd decoder and its input callback share one allocator-owned lifetime.
Decoder readback, allocation-failure and cleanup tests exercise that boundary;
an independent ownership check rejects the preceding implementation. Sanitizer
qualification includes valid-access and deliberate-overflow instrumentation
controls. These checks establish their recorded scope, not universal memory safety.

Kernel registration lives in a separately admitted first-party DLL. Native
startup requires no accelerator registration before capability detection.
Missing and altered kernel payloads have independent integrity tests: CPU
operations remain usable, while required-HIP Neutron rejects the failed module.
Compatibility and kernel DLL hashes are produced from linked bytes and checked
in staged and installed packages, independently from GPU presence.
Capability admission checks the selected GPU against the exact compiled targets
before loading the kernel module, then resolves every registered device kernel
through HIP before advertising support or allocating codec buffers. A valid
driver and intact DLL alone do not establish support for this build's kernels.

Neutron is GPU-only and separate from efforts 1–9. The
[benchmark index](benchmarks/README.md) and [public-corpus protocol](neutron-public-corpus-benchmark.md)
retain full-file sizes, actual HIP work, byte-exact readback and measurement limits.
Broad size superiority and comparative GPU speed remain research targets.

## Outstanding Acceptance Limits

- The previously reported system freeze has no confirmed cause. The provided
  watchdog dump files contain zero bytes and supply no diagnostic data. Later
  successful GPU tests cannot establish that the incident is resolved.
- Current source-analysis reports also require review even when GitHub retains
  an earlier dismissed state. The audit checks those current instances and does
  not equate a dismissal or an empty open-alert list with a production repair.
- Dependabot reports the development crawler's upstream NLTK advisory, whose
  reviewed upstream version has no officially released fix. The source-built
  downstream repair has retained original-failure and installed-consumer tests.
  It does not relabel the upstream version or remove the advisory.
- Exact candidate ZIP/MSI, checksums and installer install/repair/uninstall
  qualification must pass on the candidate's pushed SHA before release approval.
- A full version-eleven Govdocs1 study, controlled paired timings and authenticated
  live OpenVAS qualification remain outside the completed evidence.

## Candidate Qualification

The maintainer accepts the exact CodeReview and CII Best Practices observations
in the [governance baseline](../.github/scanner-governance-baseline.json) as a
pass. They remain visible in audit output and do not block release readiness.
All other source, dependency and qualification requirements remain in force.

The manual [release workflow](release.md) defaults to `publish_release=false`.
It builds and tests HIP binaries, scans source, qualifies portable/MSI payloads,
and retains checksummed artifacts without creating a tag or release. Publication
uses a separate explicitly enabled job with write permission and validates the
same artifact checksums again.

This page records acceptance boundaries, not a declaration that all gates passed.
Exact runs and final status are retained in the local handoff and GitHub checks.
Completed chronological reports remain in Git history and local review artifacts.
