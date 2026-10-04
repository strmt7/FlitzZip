# Exact corpus identity support for the scientific RAM benchmark controller.
$script:BenchmarkCorpus = $null

# Purpose: Set priority on an owned subprocess while allowing a completed process to retain its result.
# Inputs: A started process and its desired priority; completion may race with the setter.
# Outputs: Applies priority or returns only after confirmed exit; live-process errors remain fatal.
function Set-BenchmarkOwnedProcessPriority {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([Parameter(Mandatory = $true)][object]$Process,
        [Parameter(Mandatory = $true)][Diagnostics.ProcessPriorityClass]$Priority)
    if ($Process.HasExited -or -not $PSCmdlet.ShouldProcess('Owned benchmark subprocess', "Set priority to $Priority")) { return }
    try { $Process.PriorityClass = $Priority }
    catch {
        if (-not $Process.HasExited) { throw }
    }
}

# Purpose: Admit a corpus through the existing Python permission/manifest boundary within a bounded subprocess.
# Inputs: Repository root, reviewed manifest, flat input root and selected filename; no data is downloaded.
# Outputs: Returns private paths plus public provenance or throws on timeout, excess output or failed admission.
function Import-ReviewedBenchmarkCorpus {
    param([string]$RepositoryRoot, [string]$Manifest, [string]$Root, [string]$File)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'py'
    $info.Arguments = (@('-3', '-m', 'tools.benchmark_corpus', '--manifest', [IO.Path]::GetFullPath($Manifest),
        '--root', [IO.Path]::GetFullPath($Root), '--file', $File) | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '
    $info.WorkingDirectory = $RepositoryRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $started = $false
    try {
        if (-not $process.Start()) { throw 'Corpus admission process did not start.' }
        $started = $true
        Set-BenchmarkOwnedProcessPriority -Process $process -Priority BelowNormal
        $stdout = Read-BoundedBenchmarkStream -Reader $process.StandardOutput
        $stderr = Read-BoundedBenchmarkStream -Reader $process.StandardError
        if (-not $process.WaitForExit(120000)) {
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw 'Owned corpus admission process did not terminate within 5 seconds.' }
            throw 'Corpus admission exceeded its 120-second deadline.'
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $errorText) { throw "Corpus admission failed: $errorText" }
        return ($output | ConvertFrom-Json)
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
}

# Purpose: Reject link traversal, changed size or changed bytes at a frozen corpus boundary.
# Inputs: Regular-file path, expected SHA-256 and optional exact payload size.
# Outputs: Returns normally only for the same unlinked file bytes; no measured observation is discarded.
function Assert-BenchmarkCorpusFile {
    param([string]$Path, [string]$Sha256, [int64]$Bytes = -1)
    $item = Get-Item -LiteralPath $Path -Force
    $limit = if ($Bytes -ge 0) { 64MB } else { 1MB }
    if ($item.PSIsContainer -or $item.Length -lt 1 -or $item.Length -gt $limit -or
        ($Bytes -ge 0 -and $item.Length -ne $Bytes)) { throw 'Corpus file size or type changed.' }
    $ancestor = $item
    while ($null -ne $ancestor) {
        if (($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Corpus identity must not traverse links or junctions.' }
        $ancestor = if ($ancestor -is [IO.FileInfo]) { $ancestor.Directory } else { $ancestor.Parent }
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        if ($stream.Length -ne $item.Length) { throw 'Corpus file size changed before authentication.' }
        $actual = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        if ($actual -cne $Sha256) { throw 'Corpus bytes or permission metadata changed; journal retained.' }
    } finally { $algorithm.Dispose(); if ($null -ne $stream) { $stream.Dispose() } }
}

# Purpose: Freeze selected source, reviewed manifest and permission catalog around each observation.
# Inputs: Script-scoped admitted corpus, or null for generated profiles.
# Outputs: Throws on an identity change without retrying or removing recorded native evidence.
function Assert-BenchmarkCorpusIdentity {
    if ($null -eq $script:BenchmarkCorpus) { return }
    $corpus = $script:BenchmarkCorpus
    Assert-BenchmarkCorpusFile -Path $corpus.path -Sha256 $corpus.source_sha256 -Bytes $corpus.input_bytes
    Assert-BenchmarkCorpusFile -Path $corpus.manifest_path -Sha256 $corpus.provenance.manifest_sha256
    Assert-BenchmarkCorpusFile -Path $corpus.catalog_path -Sha256 $corpus.provenance.permission_catalog_sha256
}

# Purpose: Resolve natural corpus bytes or the generated workload extent without rounding or padding.
# Inputs: SizeMiB applies only when no reviewed corpus is active.
# Outputs: Exact input byte count for admission, validation and serialization.
function Get-BenchmarkInputByteCount {
    param([int64]$SizeMiB)
    if ($null -ne $script:BenchmarkCorpus) { return [int64]$script:BenchmarkCorpus.input_bytes }
    return ($SizeMiB * 1MB)
}

# Purpose: Select the independent bytewise protocol for the active source lane.
# Inputs: Script-scoped admitted corpus, or null for generated profiles.
# Outputs: Exact native protocol identifier; the two protocols are never merged.
function Get-BenchmarkMeasurementProtocol {
    if ($null -ne $script:BenchmarkCorpus) { return 'bytewise-corpus-v1' }
    return 'bytewise-regenerated-v2'
}

# Purpose: Require corpus planning to declare unverified metadata and measurements to authenticate actual bytes.
# Inputs: Native statistics and explicit planning flag; generated profiles keep their existing contract.
# Outputs: Throws on missing or mismatched corpus identity, source kind or verification state.
function Assert-BenchmarkCorpusStat {
    param([Collections.IDictionary]$Stats, [switch]$PlanOnly)
    if ($null -eq $script:BenchmarkCorpus) { return }
    $corpus = $script:BenchmarkCorpus
    if ($PlanOnly) {
        if ($Stats['data_source'] -cne 'corpus-metadata' -or $Stats['source_identity_verified'] -cne 'false' -or
            $Stats['expected_source_sha256'] -cne $corpus.source_sha256 -or $Stats.Contains('source_sha256')) {
            throw 'Corpus admission requires explicit unverified metadata matching the reviewed source.'
        }
    } elseif ($Stats['data_source'] -cne 'preloaded' -or $Stats['source_sha256'] -cne $corpus.source_sha256 -or
        $Stats['measurement_protocol'] -cne 'bytewise-corpus-v1') {
        throw 'Corpus observation identity differs from the reviewed immutable source.'
    }
}
