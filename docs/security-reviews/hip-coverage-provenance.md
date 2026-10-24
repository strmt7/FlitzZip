# HIP coverage toolchain provenance

The `revision` in [the coverage toolchain manifest](../../tools/hip-coverage-toolchain.json)
identifies [this public ROCm LLVM source commit](https://github.com/ROCm/llvm-project/commit/d6f6cb691863e9258034ab48c1dfce21689d5606).
It is an immutable dependency identifier, with no authentication capability.
The isolated compiler reports that source revision, and the corresponding source
objects were obtained from the official repository. The helper's CMake project
requires that exact Git revision and unmodified LLVM/runtime sources.

Build and qualification share this manifest. Its strict parser requires the
official repository and matching commit URL, supported typed ABI fields, unique
properties and finite numbers. The compiled helper binds the manifest, parser,
CMake recipe and C++ source hashes. Qualification refuses a stale build or another
compiler revision before admitting GPU execution. This avoids independently
maintained revision literals in executable code and build configuration.

The same manifest pins the official zlib source archive used by the development
reader, including its exact size and SHA-256. Its source URL and release version
must agree. Native compressed-profile and coverage readers require LLVM's normal
zlib support; configuration refuses a build without it. This static development
dependency is not linked into the archive application or shipped in its packages.

DevSkim's DS173237 reports highlight the manifest's quoted `revision` and zlib
archive `sha256` values. The latter matches the official release asset's published
SHA-256 and the downloaded archive's complete bytes. It is public integrity
metadata with no authentication capability.
Under the maintainer's standing authorization for individually proven false
positives, the metadata ledger admits only that exact public value and complete
manifest hash. The matcher also verifies each structured provenance role and
highlighted bytes. Changing the manifest, value or role expires admission.

Raw source and metadata reports remain retained locally. This is a public
provenance disposition, not a repaired credential exposure. Source code and tests
cannot use this metadata path; unmatched findings remain blocking. Scanner rules,
coverage, dependency pinning and GPU qualification remain enabled.
