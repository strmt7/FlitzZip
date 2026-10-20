# NLTK Upstream Distribution Provenance

The crawler installs the unmodified `nltk-3.10.4-py3-none-any.whl` published in
the official [v3.10.4-rc1 release](https://github.com/nltk/nltk/releases/tag/v3.10.4-rc1).
The tag denotes a release candidate; the upstream wheel's own version is
`3.10.4`. FlitzZip preserves both identities and does not relabel or rebuild it.
The maintainer explicitly authorized testing and adopting this candidate to
resolve the previous NLTK vulnerability.

[The distribution record](3.10.4-rc1/distribution.json) binds the official asset
URL, filename, byte size, SHA-256, release tag and license. The wheel is retrieved
into the external development cache and verified before use; built packages
are not tracked in Git. It contains the original Apache-2.0 license
and authors; the [development notice](../../../docs/licenses/NLTK-3.10.4.txt)
is byte-identical to that license.

`tools/nltk_distribution.py` verifies the complete wheel, metadata and runtime
version before installation. It binds every installed Python source and the
VERSION file to the admitted wheel without importing the package. Additional,
missing or changed source, redirects, native modules and sourceless bytecode
fail admission. Runtime commands retain the source-only import boundary.

Installation uses the complete hash-locked dependency graph, the verified local
wheel as a normal pip candidate, and `pip check`. No dependency constraint,
installed code or package metadata is patched. Regression tests cover the
model-artifact sandbox and legitimate consumer behavior. Upstream path-security
tests and actual crawler extraction are qualified separately; successful
crawling alone does not establish a security fix.

The earlier commit archive and its original record remain immutable evidence
for the individually approved historical secret-scanner fixture. The history
review binds both that archive and its Git blob. They are not installation
inputs. The retired custom NLTK builder has been removed.
