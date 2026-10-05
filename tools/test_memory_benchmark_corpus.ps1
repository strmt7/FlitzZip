param([string]$Configuration = 'Release', [switch]$RequireHip, [switch]$ControllerOnly)
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
    param([string[]]$Argument, [byte[]]$InputBytes)
    if (@($Argument | Where-Object { $_ -match '["\r\n]' -or $_.EndsWith('\') }).Count) {
        throw 'Unsupported controlled corpus fixture argument.'
    }
    $info = [Diagnostics.ProcessStartInfo]::new($cli, (($Argument | ForEach-Object { '"' + $_ + '"' }) -join ' '))
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $PSBoundParameters.ContainsKey('InputBytes')
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = Read-BoundedBenchmarkStream -Reader $process.StandardOutput
        $stderr = Read-BoundedBenchmarkStream -Reader $process.StandardError
        if ($info.RedirectStandardInput) {
            $process.StandardInput.BaseStream.Write($InputBytes, 0, $InputBytes.Length)
            $process.StandardInput.Close()
        }
        if (-not $process.WaitForExit(60000) -or -not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) {
            throw 'Corpus CLI fixture deadline exceeded.'
        }
        return @{ exit_code = $process.ExitCode; stdout = $stdout.GetAwaiter().GetResult(); stderr = $stderr.GetAwaiter().GetResult() }
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
}

# Purpose: Prove CLI binary stdin preserves all byte values and accepts natural extents beyond the former ceiling.
# Inputs: Tiny owned binary fixture, current native binary and actual HIP availability; no source file is written.
# Outputs: Exact readback succeeds and invalid source declarations/transport fail; timings are correctness-only.
function Test-MemoryCorpusStdin {
    param([bool]$Hip)
    [byte[]]$bytes = 0..255
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
    $source = @('--source-stdin', '--source-bytes', '256', '--source-sha256', $hash)
    $mode = if ($Hip) { '--neutron-star' } else { '--force-cpu' }
    $result = Invoke-MemoryCorpusProbe -Argument (@('memory-benchmark', $mode) + $source) -InputBytes $bytes
    if ($result.exit_code -ne 0 -or $result.stderr -or $result.stdout -notmatch 'validated_bytes=256 ' -or
        $result.stdout -notmatch "source_sha256=$hash " -or $result.stdout -notmatch 'memory_only=true disk_write_bytes=0') {
        throw "Binary stdin readback failed: $($result.stderr)"
    }
    $large = Invoke-MemoryCorpusProbe -Argument @('memory-benchmark', '--neutron-star', '--source-stdin', '--source-bytes',
        '175101388', '--source-sha256', $hash, '--plan-only')
    if ($large.exit_code -ne 0 -or $large.stderr -or $large.stdout -notmatch 'input_bytes=175101388 ' -or
        $large.stdout -notmatch 'source_identity_verified=false') {
        throw 'Large natural-file metadata admission failed or falsely claimed source authentication.'
    }
    $bad = @(
        @{ arguments = @('--source-stdin'); cause = 'requires --source-bytes' },
        @{ arguments = @('--source-bytes', '256'); cause = 'requires --source-stdin' },
        @{ arguments = $source + '--source-stdin'; cause = 'duplicate --source-stdin' },
        @{ arguments = $source + @('--source-bytes', '256'); cause = 'duplicate --source-bytes' },
        @{ arguments = $source + @('--profile', 'Mixed'); cause = 'excludes file/generated input' },
        @{ arguments = $source + @('--source-file', 'unused.bin'); cause = 'excludes file/generated input' },
        @{ arguments = @('--source-stdin', '--source-bytes', '0', '--source-sha256', $hash); cause = '1..4294967295' },
        @{ arguments = @('--source-stdin', '--source-bytes', '257', '--source-sha256', $hash); cause = 'read failed'; input_bytes = $bytes },
        @{ arguments = @('--source-stdin', '--source-bytes', '255', '--source-sha256', $hash); cause = 'read failed'; input_bytes = $bytes },
        @{ arguments = @('--source-stdin', '--source-bytes', '256', '--source-sha256', ('0' * 64)); cause = 'differs from expected identity'; input_bytes = $bytes }
    )
    foreach ($case in $bad) {
        $probe = @{ Argument = @('memory-benchmark', $mode) + $case.arguments }
        if ($case.ContainsKey('input_bytes')) { $probe.InputBytes = $case.input_bytes }
        $result = Invoke-MemoryCorpusProbe @probe
        if ($result.exit_code -ne 1 -or -not $result.stderr.Contains($case.cause) -or $result.stdout) {
            throw "Binary stdin rejection disagrees with expected boundary: $($case.cause)"
        }
    }
    Write-Output 'Binary stdin integration passed: exact all-byte readback, 175 MB metadata plan and ten rejection cases.'
}

# Purpose: Exercise the production controller and scheduler with this explicitly generated correctness snapshot.
# Inputs: Existing bounded fixture path/hash, owned fixture directory and actual HIP availability.
# Outputs: Requires three pilots and three confirmations per backend, exact schema-four bytes and complete journals.
function Test-MemoryCorpusController {
    param([string]$Path, [string]$Hash, [string]$Fixture, [bool]$Hip)
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/bench.ps1'), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Controller integration could not parse the production functions.' }
    foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $metadata = Join-Path $Fixture 'owned-fixture-metadata.json'
    [IO.File]::WriteAllText($metadata, '{"scope":"generated correctness fixture; no permission or performance claim"}')
    $metadataHash = (Get-FileHash -LiteralPath $metadata -Algorithm SHA256).Hash.ToLowerInvariant()
    $script:BenchmarkCorpus = [pscustomobject]@{ path = $Path; source_sha256 = $Hash; input_bytes = 16777253
        manifest_path = $metadata; catalog_path = $metadata
        provenance = @{ name = 'generated controller correctness fixture'; manifest_sha256 = $metadataHash
            permission_catalog_sha256 = $metadataHash; file = @{ name = 'owned-fixture'; transformation = 'generated correctness fixture' } } }
    try {
        $script:repo = $root; $script:SizeMiB = 10240; $script:WorkloadProfile = 'Corpus'; $script:CompressionLevel = 5
        $script:SkipCpu = $false; $SkipGpu = -not $Hip; $script:NoResourceCounters = $true
        $script:SampleIntervalMs = 50; $script:InterRunPauseMs = 0; $script:RunTimeoutSeconds = 60; $script:SuiteTimeoutSeconds = 300
        $script:BenchmarkWorkerCount = 2; $script:BenchmarkGeometryPlans = @{}; $script:BenchmarkSampleIdentities = @{}
        $script:BenchmarkDiskCounterPaths = @(); $script:BenchmarkSuiteClock = [Diagnostics.Stopwatch]::StartNew()
        $script:BenchmarkBuildReceipt = $null
        $script:NativeBuildReceiptTool = Join-Path $root 'tools/native_build_receipt.py'
        $script:BenchmarkArtifactState = Get-RamBenchmarkArtifactState -RepositoryRoot $root -BinaryPath $cli -Receipt ([ref]$script:BenchmarkBuildReceipt)
        $script:BenchmarkJournalPath = Join-Path $Fixture 'controller.jsonl'
        Write-BenchmarkJournal -Path $script:BenchmarkJournalPath -Create -Event @{ event = 'correctness_fixture'; performance_evidence = $false }
        foreach ($lane in @(Get-BenchmarkLaneOrder -Iteration 1 -SkipGpu:$SkipGpu)) {
            $flag = if ($lane -eq 'CPU') { '--force-cpu' } else { '--require-gpu' }
            $arguments = @(Get-MemoryBenchmarkArgument -ModeFlag $flag -BlockSizeKiB 8192 -RequestedDepth 1)
            $geometry = Invoke-SuperZipStat -Arguments @($arguments + '--plan-only')
            Assert-BenchmarkGeometry -Stats $geometry -Expected $geometry -PlanOnly
            $script:BenchmarkGeometryPlans["${lane}:8192"] = $geometry
        }
        $pilot = @(Invoke-BenchmarkPlannedSample -Plans @(@{ block_size_kib = 8192; confirmation_count = 3 }) -Stage pilot -JournalPath $script:BenchmarkJournalPath)
        $lanes = @(Get-BenchmarkLaneOrder -Iteration 1 -SkipGpu:$SkipGpu)
        $plans = @(Get-BenchmarkConfirmationPlan -PilotRuns $pilot -Blocks @(8192) -Lanes $lanes -MinimumCount 3 -MaximumCount 3 -MinimumSeconds 1 -TargetRsePct 2)
        $runs = @(Invoke-BenchmarkPlannedSample -Plans $plans -Stage confirmation -JournalPath $script:BenchmarkJournalPath)
        $study = ConvertTo-RamBenchmarkStudyRecord -RecordArguments @{
            Runs = $runs; Profile = 'Corpus'; SizeMiB = 10240; Level = 5; SampleIntervalMs = 50
        } -ArtifactState $script:BenchmarkArtifactState -PilotRuns $pilot -SamplingPolicy @{ method = 'correctness_fixture'; discarded_sample_count = 0 } -CaseQuality @(@{ status = 'inconclusive'; reason = 'generated correctness fixture without resource sampling' })
        $events = @(Get-Content -LiteralPath $script:BenchmarkJournalPath | ForEach-Object { $_ | ConvertFrom-Json })
        if ($study.schema_version -ne 4 -or $null -ne $study.size_mib -or $study.input_bytes -ne 16777253 -or
            $study.runs.Count -ne (3 * $lanes.Count) -or $study.pilot_runs.Count -ne (3 * $lanes.Count) -or
            @($events | Where-Object event -eq 'raw_operation').Count -ne (6 * $lanes.Count) -or
            @($events | Where-Object event -eq 'sample').Count -ne (6 * $lanes.Count)) {
            throw 'Production controller lost or relabeled a corpus correctness observation.'
        }
        foreach ($run in @($study.runs + $study.pilot_runs)) {
            if ($run.input_bytes -ne 16777253 -or $run.validated_bytes -ne 16777253 -or $run.source_sha256 -cne $Hash -or
                $run.measurement_protocol -cne 'bytewise-corpus-v1' -or -not $run.memory_only -or $run.disk_write_bytes -ne 0) {
                throw 'Production controller failed corpus source identity or RAM-only contracts.'
            }
        }
        Write-Output "Corpus controller integration passed: $($pilot.Count) pilots, $($runs.Count) frozen confirmations; correctness evidence only."
    } finally { $script:BenchmarkCorpus = $null; $script:BenchmarkSuiteClock = $null }
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
    if (-not $ControllerOnly) {
        foreach ($mode in $modes) {
            foreach ($block in @(256, 512, 1024, 2048, 4096, 8192, 16384)) {
                $arguments = @('memory-benchmark') + $source + @($mode, '--workers', '2', '--inflight', '1', '--decode-inflight', '1', '--block-size-kib', "$block", '--compression-level', '5')
                $plan = Invoke-MemoryCorpusProbe -Argument ($arguments + '--plan-only')
                if ($plan.exit_code -ne 0 -or $plan.stderr -or $plan.stdout -notmatch '^plan_only=true input_bytes=16777253 ' -or
                    $plan.stdout -notmatch 'data_source=corpus-metadata ' -or $plan.stdout -notmatch "expected_source_sha256=$hash " -or
                    $plan.stdout -notmatch 'source_identity_verified=false' -or $plan.stdout -match 'gpu_used=|seconds=|memory_only=') {
                    throw 'Corpus metadata planning must expose exact, unauthenticated configuration without measured evidence.'
                }
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
        $unverified = Invoke-MemoryCorpusProbe -Argument @('memory-benchmark', '--source-file', $path, '--source-sha256', ('0' * 64), '--force-cpu', '--plan-only')
        if ($unverified.exit_code -ne 0 -or $unverified.stderr -or $unverified.stdout -notmatch 'source_identity_verified=false' -or
            $unverified.stdout -notmatch ('expected_source_sha256=' + ('0' * 64)) -or $unverified.stdout -match 'source_sha256=\S+.*source_identity_verified=true') {
            throw 'Metadata-only planning must not claim authentication of an unchecked source hash.'
        }
        $empty = Join-Path $fixture 'empty.bin'
        [IO.File]::WriteAllBytes($empty, [byte[]]@())
        $bad = @(
            @{ arguments = @('--source-file', $path); cause = 'requires --source-sha256' },
            @{ arguments = $source + @('--profile', 'Mixed'); cause = 'excludes --size-mib' },
            @{ arguments = $source + @('--size-mib', '10240'); cause = 'excludes --size-mib' },
            @{ arguments = $source + @('--source-file', $path); cause = 'duplicate --source-file' },
            @{ arguments = @('--source-file', $path, '--source-sha256', ('0' * 64)); cause = 'differs from expected identity' },
            @{ arguments = @('--source-file', $path, '--source-sha256', $hash.ToUpperInvariant()); cause = 'lowercase SHA-256' },
            @{ arguments = @('--source-sha256', $hash); cause = 'requires a preloaded corpus' },
            @{ arguments = @('--source-file', $empty, '--source-sha256', $hash); cause = '1..4294967295' },
            @{ arguments = @('--source-file', $fixture, '--source-sha256', $hash); cause = 'regular file' }
        )
        foreach ($case in $bad) {
            $result = Invoke-MemoryCorpusProbe -Argument (@('memory-benchmark', '--force-cpu') + $case.arguments)
            if ($result.exit_code -ne 1 -or -not $result.stderr.Contains($case.cause) -or $result.stdout) {
                throw "Corpus rejection disagrees with expected boundary: $($case.cause)"
            }
        }
        Write-Output "Corpus integration passed: $($modes.Count * 7) metadata plans and backend/block cases, 9 rejection cases; GPU qualified=$hip."
    }
    Test-MemoryCorpusStdin -Hip $hip
    Test-MemoryCorpusController -Path $path -Hash $hash -Fixture $fixture -Hip $hip
    $after = Invoke-MemoryCorpusProbe -Argument @('gpu-info')
    if ($hip -and ($after.exit_code -ne 0 -or $after.stdout -notmatch 'available=true')) { throw 'HIP readiness lost after corpus integration.' }
} finally {
    $prefix = $owned.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixture).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified corpus fixture.'
    }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
