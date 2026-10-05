# NLTK Security Source Provenance

The original, unmodified upstream archive is retained under its full commit ID.
`build.json` records its official codeload URL, upstream version, exact commit,
UTC build timestamp and the merged model-artifact repairs. The adjacent SHA-256
sidecars admit that archive and the resulting canonical wheel. No generated
wheel or installed environment is tracked.

This is NLTK upstream development source at
[1bd574cfd4e3bca82c6b145d9271a646df410707](https://github.com/nltk/nltk/commit/1bd574cfd4e3bca82c6b145d9271a646df410707),
not a published NLTK release. It contains the merged repairs in
[3757](https://github.com/nltk/nltk/pull/3757),
[3759](https://github.com/nltk/nltk/pull/3759) and
[3813](https://github.com/nltk/nltk/pull/3813) for
[GHSA-8mgp-746c-j5xp](https://github.com/nltk/nltk/security/advisories/GHSA-8mgp-746c-j5xp).
The source archive contains the complete original Apache 2.0 `LICENSE.txt`,
`AUTHORS.md`, README and existing source notices.

The standard setuptools build changes only `nltk/VERSION` to the explicitly
identified downstream version and `setup.cfg` to include a prominent build
notice. It also adds `SUPERZIP_BUILD_NOTICE.txt`. Upstream Python implementation
files remain byte-exact. The wheel retains all original notices and that build
notice. Canonical packaging normalizes generated metadata line endings,
recomputes the standard wheel `RECORD`, fixes archive metadata to the upstream
timestamp and uses stored members to avoid host zlib differences. Runtime code
and original license payloads are not rewritten.

The build tools have a separate complete hash lock. Wheel construction uses
`--no-deps --no-build-isolation --no-index` in that isolated builder; runtime
installation uses the complete crawler dependency graph with normal resolution,
`--require-hashes`, binary-only selection and `pip check`. It never patches
installed code or metadata. Replace this downstream build with a compatible
patched upstream release after the source regressions and consumer checks pass.
