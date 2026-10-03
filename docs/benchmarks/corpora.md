# Reviewed Empirical Benchmark Inputs

## Household Power Measurements

On 3 October 2026 the [official UCI source](https://archive.ics.uci.edu/dataset/235/individual+household+electric+power+consumption)
identified actual household power measurements and CC BY 4.0 licensing.
Credit Georges Hebrail and Alice Berard (2006), *Individual Household Electric
Power Consumption*, UCI Machine Learning Repository,
[DOI 10.24432/C58K54](https://doi.org/10.24432/C58K54). Follow the
[license conditions](https://creativecommons.org/licenses/by/4.0/legalcode.en),
retain attribution and disclose the excerpt selection. No endorsement is implied.

The [source descriptor](data/corpora/household-power-source.json) pins the
20,640,916-byte official ZIP and its complete 132,960,755-byte member. These
hashes were observed through the official HTTPS download and independently
checked during acquisition; they are local reproducibility pins, not claimed
publisher-issued or signed checksums. The archive stays in RAM. The tool reads
the complete member, verifies its CRC and SHA-256, and retains three disjoint
20 MiB candidate windows at the beginning, middle and end. It trims partial
boundary rows, preserves literal bytes, line endings and missing-value markers,
and adds no padding, repeated data or converted numbers. The first excerpt
retains the original header; the other excerpts do not insert one.

The verified excerpts contain 20,971,465, 20,971,454 and 20,971,499 bytes:
62,914,418 payload bytes in total, below 64 MiB. `acquisition.json` records exact
source ranges, hashes and LF-delimited line counts; the first count includes
the header. Payloads remain under ignored output directories and are never
product assets or tracked files. This is one numeric time-series workload;
it does not establish performance on documents, source trees, images or
incompressible binary files.

## Repeatable Acquisition And Studies

Run acquisition through the repository's bounded, memory-admitted command
runner. Its child command is:

```powershell
py -3 -m tools.acquire_benchmark_corpus --source-pin docs/benchmarks/data/corpora/household-power-source.json --destination out/benchmarks/corpora/household-power
```

Choose a new destination. The tool rejects existing or linked destinations,
denied permissions, changed pins, redirects, wrong response sizes and wrong
container/member hashes. Chunked HTTP transfer is supported within the same
exact byte and monotonic time boundaries. Compressed acquisition is capped at
32 MiB, streamed complete-member decoding at 256 MiB, and retained payload at
64 MiB in aggregate. Source metadata is capped at one MiB. No new packages or
external extraction commands are required. The output includes attribution,
acquisition evidence and the existing permission-verified manifest schema.

After local contracts pass and source is committed, feed that manifest to the
[scientific RAM controller](../compression-level-and-benchmark-suite.md#native-suite):

```powershell
tools/bench.ps1 -CorpusManifest out/benchmarks/corpora/household-power/manifest.json -CorpusRoot out/benchmarks/corpora/household-power -CorpusFile corpus-first.txt -CompressionLevel 5 -JsonOutput out/benchmarks/household-first-native.json
```

The default covers every block size in both CPU/HIP lanes. Select a specific
`-BlockSizeKiB` for an explicitly scoped development study; repeat with middle
and last excerpts to inspect selection sensitivity. Pilot/frozen-confirmation,
resource sampling and no-discard rules remain unchanged. Short/noisy cases can
reach the iteration ceiling and remain inconclusive. Report that outcome;
do not replace the input with repetitions or hide unfavorable observations.

The same manifest supports the existing bounded ZST application diagnostic
against the reviewed unmodified Zstandard CLI. Native RAM studies and
filesystem application measurements remain separate. Acquisition success is
not a speed result, and corpus schema 4 publication still needs its separate
reviewed admission path. Additional empirical, mixed-file and small-file
corpora remain required before broader competitive claims.
