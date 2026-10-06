# Current Development Dependency Acceptance

On 2026-10-06 the maintainer explicitly accepted the currently open high-severity
NLTK advisory for this development round: "the high-severity is acceptable for
now since it cannot be solved."

This acceptance covers only [Dependabot alert 23](https://github.com/strmt7/SuperZip/security/dependabot/23),
[GHSA-8mgp-746c-j5xp](https://github.com/advisories/GHSA-8mgp-746c-j5xp), in the
isolated development crawler. It does not cover another dependency, advisory,
runtime component or later source revision. The alert remains visible.

The crawler uses `3.10.3+superzip.security1.g1bd574cfd`, built from official NLTK
GitHub source using the [pinned build record](../../third_party/upstream/nltk/1bd574cfd4e3bca82c6b145d9271a646df410707/build.json).
The pin includes merged upstream repairs beyond the published 3.10.3 release.
The downstream version distinguishes that source build; it does not pretend
that an official fixed release exists. The latest-release and advisory metadata
checked for this review still list 3.10.3 and no first patched version.

Retain the existing source-integrity, original-failure, installed-consumer and
crawler qualification evidence. Successful crawling alone does not prove an
advisory fixed. Reassess this acceptance when upstream publishes a fixed release,
updates the advisory, or the pinned source/build/dependency environment changes.
Prefer the compatible fixed official release after verifying the real consumer.
Do not generalize the earlier exceptional dependency-version override into
routine installation behavior.
