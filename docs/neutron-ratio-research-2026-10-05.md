# Neutron star mode: lossless ratio research

## Objective And Evidence

Reduce complete, self-contained SUZIP archive bytes first. Measure speed as a
secondary objective, with actual HIP encoding and decoding, bounded RAM/VRAM,
unchanged numeric levels and exact recovery of every original byte. This review
identifies implementation directions; it does not claim that these algorithms
have been implemented, that every recent paper has been exhausted, or that their
published ratios are SuperZip results. Research was checked on 2026-10-05.

The current Neutron primitive optimizes LZ4 byte costs over its admitted match
graph. It still has independent 64 KiB segments and fixed-width two-byte match
distances. Those representation limits leave substantial room for research:
additional search cannot exploit redundancy that the representation cannot
express. The existing native portfolio compares complete framed candidates and
retains its preceding winner. Future Neutron trials must retain that property.

## Primary Sources And Transferable Ideas

| Source | Relevant mechanism | Application and limits |
| --- | --- | --- |
| [ZipNN, IBM Research, CLOUD 2025](https://research.ibm.com/publications/a-lossless-compression-for-ai-models) and [maintained implementation](https://github.com/zipnn/zipnn) | Separate numeric components before applying entropy coding. | Reversible byte/bit planes can expose concentrated exponents obscured by interleaved mantissa bits. Model-weight results do not establish gains on ordinary documents or already compressed data. |
| [Heilper and Singer, August 2025](https://arxiv.org/html/2508.19263v1) | Independently code exponent/mantissa fields; XOR deltas between exact checkpoints. | Preserve sign, NaN payloads, signed zero and all mantissa bits. Delta archives need their reference bytes accounted for; a standalone archive cannot depend silently on an external checkpoint. |
| [ECF8, ICLR 2026](https://proceedings.iclr.cc/paper_files/paper/2026/hash/57c3fef21638eb9d1d67d32ba0a6d1af-Abstract-Conference.html) and [official implementation](https://github.com/zeyuyang8/ecf8) | Exploit low-entropy FP8 exponents with GPU-oriented decoding. | Lossless relative to the supplied FP8 bytes. Converting FP16/BF16 input into FP8 would change the archive's source bytes. The published implementation uses CUDA; it is not an AMD qualification. |
| [Tan et al., ISCA 2026 author manuscript](https://arxiv.org/html/2606.15789v1) | Tile-addressable ANS streams and parallel GPU decoding. | Independent integer state machines are promising for HIP entropy coding. Include stream tables, restart states, alignment and framing in size comparisons. GEMM-fusion throughput is an inference result, not an archive speedup. Broad theoretical reduction figures are not universal file-size guarantees. |
| [Nacrith, February 2026 preprint](https://arxiv.org/html/2602.19626v2) | Neural probabilities, high-precision cumulative distributions and adaptive predictors. | Promising text research, with unverified AMD portability and reproducibility obligations. Its reported text sizes exclude an approximately 500 MB decoder model, and its paper explicitly discusses training-data contamination. A self-contained archive must account for that model or an approved fixed decoder dependency. |
| [Bellard NNCP](https://bellard.org/nncp/) and [ts_zip](https://bellard.org/ts_zip/) | Transform predictions into lossless arithmetic-coded symbol streams. | Prediction is useful independently of a particular neural runtime. Sequential context, model cost, deterministic encoder/decoder probabilities and long-lived format compatibility need explicit treatment. |
| [Zstandard optimal parser](https://github.com/facebook/zstd/blob/v1.5.7/lib/compress/zstd_opt.c) and [wire format](https://github.com/facebook/zstd/blob/v1.5.7/doc/zstd_compression_format.md) | Entropy-priced literal/length/distance decisions and separate sequence streams. | A stronger Neutron representation should optimize its actual bit costs, rather than reuse LZ4's byte cost as if it described an entropy-coded stream. Re-estimate prices and compare emitted bytes after each bounded iteration. |
| [AMD HIP performance guidance, official source](https://raw.githubusercontent.com/ROCm/hip/develop/docs/how-to/performance_guidelines.rst) | Profile real bottlenecks, batch transfers and preserve necessary synchronization. | Read the official source when the rendered documentation endpoint is unavailable. Guidance from the development branch is not proof of support in the pinned SDK or across all AMD devices. |

The relevant model research includes strictly lossless methods. Quantization,
pruning, distillation and low-rank approximation can be valuable for deploying
models but change their original representation; applying them to archive
payloads would violate byte-exact recovery. A lossless entropy layer can also
compress an already quantized model without performing additional quantization.

## Ordered Neutron Experiments

The subsequent primary-source review used the installed self-hosted Crawl4AI
HTTP strategy with robots checks, without starting Chromium alongside GPU work.
The following mechanisms extend the candidate list; their published results are
not measurements of SuperZip or AMD HIP.

| Primary source | Mechanism and Neutron application | Qualification limit |
| --- | --- | --- |
| [2026 Algorithmic Information Theory Data Compression Challenge](https://arxiv.org/html/2606.17712v1), sections 2.2–2.3 and 2.9 | Combines reversible transforms with statistical coding, including adaptive pipeline selection and deterministic online context mixing. Treat classification as candidate ordering; retain measured alternatives rather than trusting an inferred file type. Fixed-point predictors are a potential route to reproducible learned coding without a large external model. | The challenge constrains memory and decoder size, requires exact recovery of every file, and separates training from testing. Its specialized generator detection is dataset-dependent evidence. Research selectors, libraries and code have not been imported or qualified here. |
| [Invariant Bit Packing for ML](https://arxiv.org/html/2605.30728v1), sections 3–4 | Learns common bit masks, keeps exceptions through participation bits, and packs varying bits with warp scans. The described implementation performs preprocessing and compression on GPU as well as GPU decompression. A Neutron trial can use integer bit patterns and count masks, exception flags, alignment and tails before selecting it. | CUDA zero-copy and ML-tensor performance are different from an AMD archive codec. Validate device capabilities and every byte, including NaN payloads and signed zero; never replace exact representation with quantization. Arbitrary archive bytes lack the paper's aligned tensor contract. |
| [NVIDIA nvCOMP Cascaded compression](https://docs.nvidia.com/cuda/nvcomp/cascaded.html) | Combines run-length, delta and frame-of-reference bit packing. Trial different reversible orders on validated integer widths and compare complete output, including expanded run metadata and remainders. | Borrow algorithmic principles, not the CUDA runtime or reference code. Width, signedness, overflow and inverse prefix scans require explicit format and HIP contracts. |
| [Meta dietgpu](https://github.com/facebookresearch/dietgpu) | Bytewise GPU rANS is an entropy layer for transformed streams, including dictionary literals and sequence components. Float exponent separation illustrates why byte/bit layout can expose redundancy hidden from an untyped byte model. | The repository is archived and its implementation targets CUDA. Published throughput is not an AMD result or a complete archive ratio. No production dependency or source import is made. |

1. Establish complete canonical-corpus baselines using the existing native RAM
   engine and Hyperfine. Report each file's complete archive bytes; preserve the
   published natural files and their order. Keep old repository-file diagnostic
   observations separate from canonical evidence.
2. Develop a Neutron-only entropy representation for literals and LZ sequence
   components. First evaluate composition with the existing HIP entropy
   portfolio, counting its tables and explicit intermediate-length metadata;
   then evaluate bounded HIP rANS streams against the existing GPU
   Huffman representation, counting all metadata. Start with byte symbols and
   independent streams before adding complex probability models. Build an
   independent reference decoder and malformed-stream regressions before
   integration. Iterative parsing must use the candidate's actual entropy prices.
   Composition requires a new versioned representation, a closed set of
   permitted inner kinds, no recursive nesting, checked intermediate extents,
   both decode stages on HIP when required, and an independent CPU reference.
   A codec that compresses only the original byte stream cannot demonstrate the
   benefit of entropy coding dictionary output.
3. Evaluate reversible byte shuffles and bit planes at widths two, four and
   eight. The transform is an exact integer permutation, including unaligned
   tails; it must not reinterpret floats or infer a type from a filename.
   Typed model experiments require validated container extents and separate
   real model checkpoints with reviewed terms. Select a transform only when its
   complete encoded block beats every preceding candidate.
4. Evaluate larger match windows and bounded shared dictionaries. The present
   LZ4 distance field cannot encode a larger window: this requires a deliberately
   versioned Neutron block representation, checked offsets and explicit restart
   ownership. Cross-file redundancy must be measured using a separately defined
   multi-entry workload; independent-file results cannot prove solid-archive gains.
5. Evaluate context predictors and neural entropy models only after the simpler
   reversible/entropy foundations have measured results. Require fixed model
   provenance, full decoder/model byte costs, post-training-cutoff holdouts,
   deterministic probability/CDF generation across qualified AMD hardware,
   bounded work and explicit approval for any new runtime dependency.

These are prioritized experiments, not a promise of dramatic improvement on
every file. Large gains need structure that the new representation can exploit;
already compressed or high-entropy data may retain the preceding winner.

## Acceptance Before Product Integration

New representations need their own version/capability contract and rejection by
older readers. Retain backward decoding, required-HIP encode/decode telemetry,
CPU reference readback, corrupt/truncated/oversized-stream tests, cancellation
and workspace cleanup. No format marker may be reused for incompatible bytes.

Evaluate each round against the previous Neutron source and the same exact
corpus, settings and tool identities. Count all selected and discarded HIP work.
Reject per-file archive-size regressions; investigate apparent timing gains
under shared-host noise. Ordinary levels 1–9 remain unchanged. A later transfer
requires both smaller complete archives and higher speed at the affected level,
plus its direct-consumer and compatibility checks.
