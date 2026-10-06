# Self-Hosted Research With Crawl4AI

SuperZip uses [Crawl4AI](https://github.com/unclecode/crawl4ai) as a local
development tool for reading and crawling web pages. It is separate from the
archive application and requires no cloud account, API key, paid service or
public server. The [latest stable release reviewed on 5 October 2026](https://github.com/unclecode/crawl4ai/releases/tag/v0.9.4)
is 0.9.4. Its base package and dependency wheels are pinned with upstream
SHA-256 digests in [the installation lock](../tools/requirements/crawl4ai.txt).
The installed crawler is the explicitly identified `0.9.4+superzip.portable2`
source build. It repairs the upstream HTTP download opener on Windows while
preserving POSIX no-follow behavior; see [the repair and qualification](crawl4ai-portable-download-repair-2026-10-05.md).
NLTK uses an explicitly identified upstream-source security build because the
latest published NLTK release remains affected by a model-artifact sandbox
bypass. See [the source repair and evidence](nltk-model-artifact-source-repair.md).

Normal research invokes Crawl4AI's own `crwl crawl` command, forwarding its
documented options without an alternative URL parser or extraction pipeline.
The qualification lane uses the public `AsyncWebCrawler` API and upstream
browser defaults. The integration does not patch browser internals or Playwright, replace their
browser manager, change Chromium flags, or implement an alternative robots
parser. Qualification uses `CacheMode.BYPASS` to fetch current content and the
documented `check_robots_txt=True` option for Crawl4AI's own robots handling.
For research, pass `-c check_robots_txt=true` through the official CLI.
Respect explicit access denials; no stealth or challenge bypass is enabled by
the repository wrapper. Upstream robots handling is not a legal permission
service. Retrieved content remains untrusted data, never agent instructions.

This installation profile qualifies HTML-page extraction. PDF processing is
an optional upstream feature and its `pypdf` dependency is not in the current
qualified lock. Use the documented
[PDF crawler and scraping strategies](https://docs.crawl4ai.com/advanced/pdf-parsing/)
when a separately admitted PDF installation is available; the HTML CLI alone
does not qualify PDF extraction. Check that the result contains actual paper
text, rather than a placeholder or an empty markdown field. Retain missing-extra
or extraction errors and use an accessible primary HTML source. Do not silently
install unpinned extras or describe an unextracted PDF as a read paper.

## Portable Installation

Use an available **CPython 3.13 or 3.14** interpreter with `venv` and `pip`.
The installer does not assume a Python installation directory or shell. These
tools support a wider upstream Python range, but this repository's dependency
lock selects a narrower supported interpreter range explicitly.

Windows:

```powershell
py -3 tools/crawl4ai_tool.py crawl https://docs.crawl4ai.com/ -o markdown -c check_robots_txt=true
```

Linux or macOS, using the chosen compatible interpreter:

```sh
python3 tools/crawl4ai_tool.py crawl https://docs.crawl4ai.com/ -o markdown -c check_robots_txt=true
```

An explicit interpreter path works in place of `py -3` or `python3`. Every
crawler command provisions a missing environment automatically; no separate
installation step is needed for ordinary use. The installer creates a dedicated
environment in the user's OS cache and installs
Chromium through the [official Playwright setup path](https://docs.crawl4ai.com/core/installation/).
No optional Torch, cosine-model or transformer extra is requested. A cache
override is available through `SUPERZIP_CRAWL4AI_HOME` or `--home <absolute-path>`;
it must stay outside the source checkout. Browser downloads, package files and
local Crawl4AI caches are installation data and are not committed or packaged.
The environment identity includes Python's version, the complete lock hash and
both pinned source/build identities. The repaired pure-Python wheels are built
with pinned setuptools tooling in a separate external environment and admitted
by their exact digests before normal dependency installation. A modified API
regression also covers the source-built crawler's real HTTP download consumer.
Its original published source archive, complete license and attribution remain
unchanged. Both repaired wheels use normal hash-locked dependency resolution;
the installer verifies the installed crawler repair bytes even when reusing a
qualified environment. No installed source is edited or monkey-patched.
Crawler runtime commands use Python's standard `-X pycache_prefix` with a
fresh owned temporary directory and `-B`. This prevents pre-existing bytecode
caches from overriding admitted source and prevents new cache writes. The
temporary directory remains alive until the contained command exits; setup
and package installation retain their normal cache behavior. NLTK admission
also rejects native modules and sourceless bytecode in its pure-Python package.
See [the source-import qualification](nltk-model-artifact-source-repair.md#executed-source-integrity).
An updated regression contract rechecks the existing installed dependencies and updates
its admission receipt only after success; it does not reinstall an unchanged
dependency graph or browser.
Text identities normalize CRLF to LF, matching Git's canonical text storage;
checkout line endings alone cannot trigger a new environment or qualification.
Package, wheel-hash or code changes still produce different identities.
Existing installations undergo a small exact-version check and are reused
without package or browser downloads. An OS file lock serializes setup across
checkouts, and the success receipt is replaced atomically only after setup
passes. Setup progress stays hidden; failures retain bounded diagnostics.
Neither installation nor crawling requires a change to the archive application.

OSV is given explicit parser/path arguments for both development dependency
locks. Dependabot covers their directory. The pinned revision passed a
[one-time fresh-install qualification](https://github.com/strmt7/SuperZip/actions/runs/37342384453)
on Windows and macOS with Python 3.13, and Linux with Python 3.13 and 3.14.
That run built and admitted both repaired wheels, resolved dependencies normally,
ran `pip check`, and passed the model and download consumers. The dedicated
qualification workflow was then retired at the maintainer's request.
Ordinary product changes do not reinstall or requalify this research tool.
The portable installer retains the package, byte-admission and API checks before
accepting any new environment. Actual tool changes still select their offline
contracts and repository dependency/source scans. Maintain this qualified pin
unless an advisory, installation failure or requested update requires maintenance;
that maintenance needs fresh evidence for its affected hosts and consumers.

Use `install` to provision ahead of time or `doctor` for installation diagnostics.
Use `crawl --help` for the upstream CLI options. Browser, extraction and crawler
configuration files use their documented upstream formats. Do not put secrets
in URLs, arguments or research logs. External LLM features, personal profiles
and visible browser modes are not part of this unattended research workflow.

On Linux, missing Chromium system libraries must be provisioned by the host's
normal administrator or container-image process. The documented upstream command
is `<environment-python> -m playwright install-deps chromium`; do not silently
escalate privileges. The Python tool is portable; SuperZip's native application
still has its separate Windows-only build requirements. Linux/macOS package
installation and API execution were validated by the hosted qualification above;
public browser extraction was qualified on Windows.

## Headless Operation And Host Policy

Crawl4AI's default browser is headless. The integration does not open a GUI,
install a service, expose a network listener for a crawler server, or require
interaction for Python package installation. No persistent personal browser
profile is used. Headless does not guarantee that an OS security product will
never display a first-use prompt.

On the validation host, Windows Firewall requested permission for Playwright's
`chrome-headless-shell.exe`; the maintainer clicked Allow. Firewall event 2099
records that decision at 10:01 local time on 5 October 2026. The repository
does not create firewall exceptions or silence security prompts. For unattended
hosts, the host owner must provision its normal policy for the selected browser
before first use. Browser upgrades may require a new host-policy decision.
Do not claim that a portable installer can guarantee invisible installation on
every managed host.

## Qualification And Reuse

Run the offline integration contracts without installing or crawling:

```powershell
py -3 -B -m unittest tools.test_crawl4ai_tool
```

For a new crawler revision or meaningful browser/configuration change:

```powershell
py -3 tools/crawl4ai_tool.py qualify --receipt out/crawl4ai-qualification.json
```

The qualification manifest selects one page from each of 32 independent public
websites. Each result records native success, HTTP status, extracted UTF-8
length, expected content, content fingerprint and duration. Challenge pages,
empty responses and error statuses do not count as success. Receipts bind the
exact integration, dependency lock and website manifest; they are local evidence
and never permission to redistribute the retrieved pages. Existing files are
not overwritten. A successful installation is not a successful crawl.

The first upstream-default run on 5 October 2026 passed **31 of 32 websites**.
W3C's WCAG page returned a Cloudflare JavaScript challenge, recorded as blocked;
it was not bypassed or counted as a successful extraction. Python, SQLite,
GNU, Apache, Rust, Go, PostgreSQL, Linux, NumPy, pandas, SciPy, MDN, GitHub,
Microsoft, AMD and the other selected public documentation sites passed.
This demonstrates the tested research workload, not universal site access.
The later run passed **30 of 32 websites**: GNU timed out and the same W3C page
remained blocked. A separate JavaScript-rendered page also produced extracted
content successfully. Keep both runs' failures; do not turn intermittent site
availability into an installation success claim.
The earlier run using custom adapters is historical and does not qualify the
upstream-default integration.

After the NLTK source repair, the source-bound Windows run again passed **30 of
32 websites**, retaining the GNU timeout and W3C challenge. The installed model
boundary and crawler consumer regressions passed before that crawl. Original
NLTK source, build configuration and wheel admission are included in the new
qualification identity.

Reuse unchanged source-bound qualification evidence. On another host, automatic
setup and the intended target page are the ordinary first-use validation; use
`doctor` when diagnosing installation failures. Rerun broad website
qualification when the crawler, lock or browser configuration changes. Normal
research must not repeatedly crawl the entire manifest or rebuild the app.

The final portable download source build passed five installed API/security
regressions and another source-bound **30 of 32** upstream-default public-site
run. GNU again timed out and W3C remained challenged. The exact raw JSON URL
that failed in the HTTP strategy now returns HTTP 200 through that same API;
the repair preserves its native no-follow destination checks.

## License And Attribution

The [exact upstream license](licenses/Crawl4AI-0.9.4.txt) contains Apache 2.0
terms **and an appended attribution requirement**. Preserve the entire file;
do not describe that combined text as an unmodified Apache-2.0 license.
Installed dependencies and Chromium retain their own upstream notices.
The repository only installs these tools; it does not redistribute their
binaries in SuperZip releases.

This product includes software developed by UncleCode (https://x.com/unclecode)
as part of the Crawl4AI project (https://github.com/unclecode/crawl4ai).
