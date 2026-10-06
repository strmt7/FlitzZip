# Canterbury Neutron baseline, 5 October 2026

The complete original eleven-file [Canterbury corpus](https://corpus.canterbury.ac.nz/descriptions/)
was measured as independent unmodified files through Hyperfine 1.20.0 and the
production required-HIP Neutron star mode. Attribute Ross Arnold and Timothy
Bell, *A corpus for the evaluation of lossless compression algorithms*, DCC
1997. This established, older corpus is a baseline; it does not represent every
modern document, model checkpoint or already compressed workload.

The HIP build corresponds to product source commit
`e6cc50f12207553b34d57301dc0ca28dec6bf92f`, native input commitment
`4f6b92d42c7372b37e0c05c6839b1ac252e5cd275e27cb836b1fb9de8a508d9f`
and receipt commitment
`c0e2e8941dbe168670c921711f6a9ceb9b699086217c4bd3144b13efff3607e0`.
The controller's repaired admission contract distinguishes compressed payload
bytes from complete restored byte coverage. Its normal resolution, required-HIP
dispatch, exact source hashes and bytewise readback remain enforced.

All 11 published files were admitted, with no exclusions, splitting, padding,
normalization or substitute content. Three full runs produced 33 validated
observations at a 256 KiB block size. Every aggregate run contains 2,810,784
original input bytes and 1,008,754 complete framed archive bytes: 35.8887% of
input size. Payloads, archives and transport mappings remained in RAM;
`memory_only=true` and `disk_write_bytes=0` were enforced on every observation.

| Original file | Input bytes | Complete archive bytes |
| --- | ---: | ---: |
| alice29.txt | 152089 | 69931 |
| asyoulik.txt | 125179 | 62508 |
| cp.html | 24603 | 10405 |
| fields.c | 11150 | 4319 |
| grammar.lsp | 3721 | 1835 |
| kennedy.xls | 1029744 | 329145 |
| lcet10.txt | 426754 | 187480 |
| plrabn12.txt | 481861 | 254604 |
| ptt5 | 513216 | 69599 |
| sum | 38240 | 16410 |
| xargs.1 | 4227 | 2518 |

The table matches every file in the first and third complete runs. The second
run's aggregate archive size is identical. The bounded tool-output capture
retained 24 raw observations, including all first and third run records and
two second-run records; the controller's completed summary retains totals for
all three runs and confirms all 33 were validated. The retained numerical
evidence is `out/neutron-canterbury-e6cc50f.json`. No corpus payload is tracked
or redistributed.

Hyperfine reports a 17.720-second mean for the complete eleven-file feeder
run, including process and transport overhead. Per-run native compression
totals were 12.4649, 12.4679 and 12.4177 seconds. These timings are explicitly
unqualified for shared-host isolation and do not prove a GPU speedup or a
general performance ranking. Complete archive bytes are the primary baseline.

This run also exposed two controller/transport failures. The controller had
required compressed output size to equal original input size, which rejected
valid compression. Its contracts now accept both shrinking and expanding
payloads, require complete bytewise validation, and reject payloads exceeding
their framed archives. Hyperfine forwards worker diagnostics without ignoring
failures. Separately, hosted binary-stdin tests inherited a preamble-capable
text encoding. An independent process reproduced 259 transported bytes for a
256-byte fixture. The owned binary adapter now prevents that preamble and
restores ambient state; actual CPU and HIP stdin consumers pass under both
Windows PowerShell and PowerShell 7 with the failure trigger enabled.

The larger Govdocs published thread, ratio algorithm iterations and full
hosted acceptance remain separate work. The system-freeze cause remains
unresolved; successful finite runs do not establish universal stability.
