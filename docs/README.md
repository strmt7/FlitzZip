# FlitzZip Documentation

FlitzZip is a Windows archive manager with a native GPU-accelerated FLZIP format
and support for common archive formats. Start with the guide for your task.

## Get started

- [Install FlitzZip](installing.md): requirements, portable ZIP, MSI and checksums.
- [Create, extract and verify archives](using-flitzzip.md): the graphical workflow,
  compression settings, Neutron star mode and operation history.
- [Use the command line](cli.md): examples, options, filename encoding and exit codes.
- [Troubleshoot a problem](troubleshooting.md): GPU availability, unsupported
  archives, output errors and useful diagnostics.

## Formats and capabilities

- [Format support](archive-format-support.md) lists supported operations, methods
  and restrictions for each format.
- [Native FLZIP format](native-flzip-format.md) describes version compatibility
  and the on-disk representation.
- [Security design](security.md) explains archive validation, publication and
  optional integrity and Defender checks.
- [Third-party software](third-party.md) identifies bundled components and notices.

## Performance and limitations

- [Benchmark evidence](benchmarks/README.md) identifies measured workloads,
  complete archive sizes and comparison limits.
- [Neutron star mode](neutron-star-mode-research.md) explains the separate
  minimum-size search and its current scope.
- [Release scope and known limitations](beta-readiness.md) identifies the
  candidate's acceptance boundaries and unresolved historical observations.
- [Release notes](releases/0.8.0.md) describe the current candidate's changes.

Report reproducible problems through [GitHub Issues](https://github.com/strmt7/FlitzZip/issues).
Use the [security reporting policy](../SECURITY.md) for suspected vulnerabilities.

## Development

Contributors and maintainers should start with the
[developer guide](development/README.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
Build internals, testing procedures, research tools and release automation are
documented there. AI agents additionally follow [AGENTS.md](../AGENTS.md).
