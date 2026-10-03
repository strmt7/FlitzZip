param([string]$Configuration = 'Release', [switch]$RequireHip)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $root "build/$Configuration/superzip_cli.exe"
. (Join-Path $PSScriptRoot 'benchmark_statistics.ps1')
$receiptArguments = @('-3', (Join-Path $PSScriptRoot 'native_build_receipt.py'), 'validate', '--root', $root, '--configuration', $Configuration)
if ($RequireHip) { $receiptArguments += '--require-hip' }
$receiptOutput = & py @receiptArguments
if ($LASTEXITCODE -ne 0) { throw 'Corpus integration requires a current successful build receipt.' }
$receipt = ($receiptOutput -join "`n") | ConvertFrom-Json
Write-Output "Corpus integration receipt: $($receipt.receipt_sha256)."

# Purpose: Capture bounded corpus CLI success or failure without interpreting timings as performance evidence.
# Inputs: Controlled argument strings contain no quotes/newlines or trailing backslash; Cli is the current binary.
# Outputs: Returns status and complete streams bounded to 65536 characters each; kills its owned process on timeout.
function Invoke-MemoryCorpusProbe {
    param([string[]]$Argument)
    if (@($Argument | Where-Object { $_ -match '["\r\n]' -or $_.EndsWith('\') }).Count) {
        throw 'Unsupported controlled corpus fixture argument.'
    }
    $info = [Diagnostics.ProcessStartInfo]::new($cli, (($Argument | ForEach-Object { '"' + $_ + '"' }) -join ' '))
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = Read-BoundedBenchmarkStream -Reader $process.StandardOutput
        $stderr = Read-BoundedBenchmarkStream -Reader $process.StandardError
        if (-not $process.WaitForExit(60000) -or -not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) {
            throw 'Corpus CLI fixture deadline exceeded.'
        }
        return @{ exit_code = $process.ExitCode; stdout = $stdout.GetAwaiter().GetResult(); stderr = $stderr.GetAwaiter().GetResult() }
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
}

$owned = [IO.Path]::GetFullPath((Join-Path $root 'out/memory-corpus-tests'))
$fixture = Join-Path $owned ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
try {
    $path = Join-Path $fixture ('source with spaces ' + [char]0x03B1 + '.bin')
    & py -3 -c 'import hashlib,pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(hashlib.shake_256(bytes(range(256))).digest(16777253))' $path
    if ($LASTEXITCODE -ne 0) { throw 'Corpus fixture creation failed.' }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $source = @('--source-file', $path, '--source-sha256', $hash)
    $gpu = Invoke-MemoryCorpusProbe -Argument @('gpu-info')
    if ($gpu.exit_code -notin @(0, 1) -or $gpu.stderr -or $gpu.stdout -notmatch 'hip_compiled=(true|false)' -or
        $gpu.stdout -notmatch 'available=(true|false)') { throw 'Corpus GPU readiness query failed.' }
    $hip = $gpu.exit_code -eq 0 -and $gpu.stdout -match 'hip_compiled=true' -and $gpu.stdout -match 'available=true'
    if ($RequireHip -and -not $hip) { throw 'Corpus integration requires actual compiled and available HIP.' }
    $modes = @('--force-cpu')
    if ($hip) { $modes += '--require-gpu' }
    foreach ($mode in $modes) {
        foreach ($block in @(256, 512, 1024, 2048, 4096, 8192, 16384)) {
            $arguments = @('memory-benchmark') + $source + @($mode, '--workers', '2', '--inflight', '1', '--decode-inflight', '1', '--block-size-kib', "$block", '--compression-level', '5')
            $result = Invoke-MemoryCorpusProbe -Argument $arguments
            if ($result.exit_code -ne 0 -or $result.stderr -or
                $result.stdout -notmatch 'input_bytes=16777253 ' -or $result.stdout -notmatch 'validated_bytes=16777253 ' -or
                $result.stdout -notmatch "source_sha256=$hash " -or $result.stdout -notmatch 'measurement_protocol=bytewise-corpus-v1 ' -or
                $result.stdout -notmatch 'data_source=preloaded ' -or $result.stdout -notmatch 'memory_only=true disk_write_bytes=0') {
                throw "Corpus integration failed: $mode block=$block exit=$($result.exit_code) $($result.stderr)"
            }
            $expectedGpu = if ($mode -eq '--require-gpu') { 'true' } else { 'false' }
            if ($result.stdout -notmatch "gpu_used=$expectedGpu ") { throw 'Corpus backend identity disagrees with requested lane.' }
            if ($mode -eq '--require-gpu' -and ($result.stdout -notmatch 'gpu_encode_chunks=[1-9][0-9]* ' -or
                    $result.stdout -notmatch 'gpu_decode_chunks=[1-9][0-9]* ' -or $result.stdout -notmatch 'gpu_kernel_launches=[1-9][0-9]* ')) {
                throw 'Corpus required-HIP lane must report actual encode/decode work and kernel launches.'
            }
            Write-Output "Corpus correctness passed: mode=$mode block_kib=$block exact_bytes=16777253; timings are unqualified."
        }
    }
    $empty = Join-Path $fixture 'empty.bin'
    [IO.File]::WriteAllBytes($empty, [byte[]]@())
    $bad = @(
        @{ arguments = @('--source-file', $path); cause = 'requires --source-sha256' },
        @{ arguments = $source + @('--profile', 'Mixed'); cause = 'excludes --size-mib' },
        @{ arguments = $source + @('--size-mib', '10240'); cause = 'excludes --size-mib' },
        @{ arguments = $source + @('--plan-only'); cause = 'excludes --size-mib' },
        @{ arguments = @('--source-file', $path, '--source-sha256', ('0' * 64)); cause = 'differs from expected identity' },
        @{ arguments = @('--source-file', $path, '--source-sha256', $hash.ToUpperInvariant()); cause = 'lowercase SHA-256' },
        @{ arguments = @('--source-sha256', $hash); cause = 'requires a preloaded corpus' },
        @{ arguments = @('--source-file', $empty, '--source-sha256', $hash); cause = '1..67108864' },
        @{ arguments = @('--source-file', $fixture, '--source-sha256', $hash); cause = 'regular file' }
    )
    foreach ($case in $bad) {
        $result = Invoke-MemoryCorpusProbe -Argument (@('memory-benchmark', '--force-cpu') + $case.arguments)
        if ($result.exit_code -ne 1 -or -not $result.stderr.Contains($case.cause) -or $result.stdout) {
            throw "Corpus rejection disagrees with expected boundary: $($case.cause)"
        }
    }
    $after = Invoke-MemoryCorpusProbe -Argument @('gpu-info')
    if ($hip -and ($after.exit_code -ne 0 -or $after.stdout -notmatch 'available=true')) { throw 'HIP readiness lost after corpus integration.' }
    Write-Output "Corpus integration passed: $($modes.Count * 7) backend/block cases, 9 rejection cases; GPU qualified=$hip."
} finally {
    $prefix = $owned.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixture).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified corpus fixture.'
    }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
