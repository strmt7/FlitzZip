# Use FlitzZip

The Queue holds the files, folders and archives selected for an operation.
Use **Add files**, **Add folder**, or drag local items into the Queue.
Checkboxes determine which items participate; selecting a row and checking it
are separate actions.

## Create an archive

1. Add the source files or folders to the Queue and check the items to include.
2. Open **Compress**, choose the destination and archive name, and select a format.
3. Choose the available compression options and start the operation.
4. Check the result in **History**. Keep the source files until you have verified
   the archive and restored important contents independently.

Only supported creation formats appear in the format selector. Options that do
not apply to a format are disabled. The
[format matrix](archive-format-support.md) describes each format's capabilities.
Compatibility formats use CPU adapters; native FLZIP provides the GPU path.

## Choose compression settings

The numeric efforts trade computation for a larger compression search budget.
A higher effort does not guarantee a smaller archive for every input. Small or
already-compressed files can gain little and may grow because an archive also
stores metadata.

For native FLZIP, the compression method selects the required or preferred HIP
policy. Required HIP reports an error if the requested work cannot run on the
GPU. Preferred HIP permits the documented CPU path.

### Neutron star mode

Neutron is separate from efforts 1–9 and prioritizes the smallest complete
representation found by its additional GPU search. It appears below effort 9
only when a compatible AMD HIP GPU is detected and the required-GPU setting is
enabled. It always requires real GPU execution.

This mode can take substantially more time. Its benefit depends on the workload;
read the [measured results](neutron-public-corpus-benchmark.md) and
[mode description](neutron-star-mode-research.md) before choosing it for a large job.

## Extract an archive

Add the archive to the Queue, open **Extract**, choose the destination and start
the operation. FlitzZip detects supported formats automatically. Batch extraction
uses separate destination directories and resolves conflicting archive names.

The default overwrite policy asks before replacing existing files. Review the
destination and replacement choice. Malformed metadata, unsafe paths and
unsupported methods produce an error. A failed operation can still require
checking its reported per-file result; do not infer success from a partially
populated destination.

## Verify and inspect a result

The **Security** page verifies supported FLZIP, ZIP and ZIPX contents and shows
the operation's validation result. Unsupported verification methods fail
explicitly. **Verify FLZIP/ZIP after write** adds validation before a new archive
is published.

SHA-256 hashing and Microsoft Defender scanning are optional checks. Hashing
detects byte changes when compared with a trusted reference. Defender scanning
requires the corresponding Windows service; a failed or timed-out scan is
reported as a failure. These options do not establish that every archive is safe.

## Progress, cancellation and settings

The status bar reports operation progress. Cancellation requests stop work
cooperatively and preserve the reported completion state; allow cleanup to finish
before starting a replacement operation. **History** records successes, failures
and the associated details.

Change preferences on **Settings**, then choose **Apply** to persist them.
**Show operation summary** opens the relevant History result after an operation.
The **System** page reports GPU availability and current CPU, GPU, RAM and I/O
observations; an unavailable reading is distinct from an observed zero.

For a scriptable workflow, use the [CLI guide](cli.md). For an error, see
[troubleshooting](troubleshooting.md).
