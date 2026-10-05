# Portable Crawl4AI Download Source Repair

The normal research launcher already read the affected raw GitHub JSON URL
successfully. The failing consumer was Crawl4AI 0.9.4's optional
`AsyncHTTPCrawlerStrategy`: it downloads non-HTML responses through
`_nofollow_opener`, which unconditionally evaluates `os.O_NOFOLLOW`. Windows
does not expose that POSIX flag. The returned failure was
`HTTP request failed: module 'os' has no attribute 'O_NOFOLLOW'`.

The [latest official release](https://github.com/unclecode/crawl4ai/releases/tag/v0.9.4)
was checked on 5 October 2026. Rather than removing no-follow protection, changing
the URL or editing an installed package, the integration builds the separately
identified `0.9.4+superzip.portable1` wheel from the immutable published source.
[The recipe](../third_party/upstream/crawl4ai/0.9.4/build.json), source digest,
reviewed wheel digest and normal dependency lock identify its provenance.
Upstream's complete license, appended attribution and a modified-source notice
are retained in the standard wheel. It remains a development dependency and
is not shipped in the archive application.

The source delta is one download-opener delegation, its new portable helper,
the downstream version and license-notice packaging. All other runtime source
files remain byte-identical to the original source archive. The existing pinned
standard packaging toolchain and canonical wheel writer are reused. Upstream's
release-tag commit time supplies the reproducible wheel timestamp. Upstream's
build-time cache setup runs inside the builder's owned temporary source tree;
it cannot remove a personal Crawl4AI cache.

On Windows, the opener uses
[CreateFileW with FILE_FLAG_OPEN_REPARSE_POINT](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
It opens without truncation, checks the actual handle's attributes, transfers
ownership to a non-inheritable Python descriptor, requires a singly linked
regular file, and only then truncates. Reserved Windows names and alternate
data streams are rejected. Every rejected handle or descriptor is closed.
POSIX retains its original `O_NOFOLLOW` open. This preserves the final-component
boundary; the download root and its ancestors remain trusted local configuration,
as in upstream's original contract. It does not claim protection against an
administrator replacing the entire ancestor tree.

The public upstream CLI, browser defaults, robots handling, TLS, URLs, request
configuration, extraction and dependency constraints are unchanged. No fake
`O_NOFOLLOW` constant, catch-and-ignore path, browser hook, installed-source edit
or security exception is used. Cached installation verifies the exact installed
strategy, helper and version bytes; changing source or test identities requires
their affected admission checks. A future upstream release can replace this
downstream build only after its opener and consumers pass the same regressions.

The retained regression exercises real creation, shorter overwrite, exclusive
creation, descriptor ownership, a symbolic link without target truncation,
Windows hardlinks/device names/alternate streams, and the actual HTTP JSON
consumer through an owned local server. The offline source contracts cover
exact source and license preservation, source/hash tampering, archive traversal
and links, repeat repair refusal, and real Windows failure cleanup. Hosted
Windows, Linux and macOS lanes build and admit the same wheel, resolve all
dependencies normally, and run both the NLTK and download consumers. Browser
qualification is a separate source-bound public-site run; pending or blocked
sites must not be counted as success.

The final Windows package passed all five installed download regressions,
including the wrong-byte cache-admission rejection control, and the nine
existing NLTK/consumer regressions. The original raw GitHub JSON URL returned
HTTP 200 through the repaired `AsyncHTTPCrawlerStrategy`, with the expected
CodeQL version fields and a successfully written source download. An HTML
Microsoft API page also returned HTTP 200 through that same strategy. The
source-bound browser qualification passed 30 of 32 independent public sites;
GNU timed out and W3C's Cloudflare challenge remained blocked. Neither failed
site was bypassed or counted as success. Raw local qualification receipts retain
both runs and their exact source/configuration identities.

The hosted platform matrix remains a separate qualification gate. A local
Windows pass does not establish Linux/macOS execution until those jobs pass.
