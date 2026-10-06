# Beta Readiness

The configured candidate is SuperZip 0.8.0 for Windows 11 x64, built with HIP.
A candidate build and its validation are separate from permission to publish.

## Product Evidence

The retained native qualification covers 626 main tests, seven CTest suites,
required-HIP and CPU readers across all seven block settings, binary benchmark
input and the frontend's Neutron capability/settings gates. Local reuse requires
the successful native receipt to match current inputs, configuration and outputs.
Hosted compilation covers the six release HIP targets; it does not prove runtime
behavior on every AMD device.

Neutron is GPU-only and separate from efforts 1–9. The
[benchmark index](benchmarks/README.md) and [public-corpus protocol](neutron-public-corpus-benchmark.md)
retain full-file sizes, actual HIP work, byte-exact readback and measurement limits.
Broad size superiority and comparative GPU speed remain research targets.

## Outstanding Acceptance Limits

- The previously reported system freeze has no confirmed cause. The provided
  watchdog dump files contain zero bytes and supply no diagnostic data. Later
  successful GPU tests cannot establish that the incident is resolved.
- The strict hosted audit still reports the previously accepted CodeReview and
  CII Best Practices governance observations. They remain visible and unresolved;
  source changes cannot manufacture peer review or external certification.
- Dependabot reports the development crawler's upstream NLTK advisory, whose
  reviewed upstream version has no officially released fix. The source-built
  downstream repair has retained original-failure and installed-consumer tests.
  It does not relabel the upstream version or remove the advisory.
- Exact candidate ZIP/MSI, checksums and installer install/repair/uninstall
  qualification must pass on the candidate's pushed SHA before release approval.
- A full version-eleven Govdocs1 study, controlled paired timings and authenticated
  live OpenVAS qualification remain outside the completed evidence.

## Candidate Qualification

The manual [release workflow](release.md) defaults to `publish_release=false`.
It builds and tests HIP binaries, scans source, qualifies portable/MSI payloads,
and retains checksummed artifacts without creating a tag or release. Publication
uses a separate explicitly enabled job with write permission and validates the
same artifact checksums again.

This page records acceptance boundaries, not a declaration that all gates passed.
Exact runs and final status are retained in the local handoff and GitHub checks.
Completed chronological reports belong in [documentation history](history/README.md).
