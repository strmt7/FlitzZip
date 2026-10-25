# Semgrep Finding Gate Review

Semgrep completed its full scan and uploaded two source findings while the
`--error` exit status made its CI job fail. Those exact reports have individual
current-source reviews in the ledger; the scanner's finding exit status alone
cannot distinguish them from a new defect.

The workflow keeps `--error`, the same rule sets, source scope, JSON capture,
coverage audit and raw SARIF upload. It preserves exit status 1 for the exact
finding gate and fails scanner errors. The gate independently compares every
JSON and SARIF finding by scanner version, rule, path and complete location;
it checks the committed review and every live context file. New, duplicated,
altered, unreviewed or malformed results fail the job. The scanner's raw output
remains available even when the gate rejects it.

The workflow's parsed YAML change affects only `jobs.semgrep`: all top-level
fields and every other job compare equal to the preceding committed workflow.
Existing CodeQL and DevSkim source, caller and scanner assumptions therefore
remain unchanged while their whole-file workflow context hashes are rekeyed.
This comparison does not approve a different finding or scanner configuration.
The direct gate regressions exercise a reviewed finding, new and duplicate
findings, mismatched outputs and exit statuses, and changed live evidence.
