param([switch]$AllRuntimes)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'benchmark_statistics.ps1')
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'test_memory_benchmark_corpus.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Cannot parse the actual binary corpus adapter.' }
$definition = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false) |
    Where-Object Name -eq 'Invoke-MemoryCorpusProbe'
if (@($definition).Count -ne 1) { throw 'Actual binary corpus adapter is missing or ambiguous.' }
$original = $definition.Extent.Text
. ([scriptblock]::Create($original))

# Purpose: Exercise the actual corpus stdin consumer's backend selection without launching product code.
# Inputs: The parsed production test function and an isolated capability-aware CLI probe double.
# Outputs: Both capability paths reach the rejection controls; the original unconditional Neutron plan fails.
function Test-CorpusPlanningBackend {
    $consumer = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false) |
        Where-Object Name -eq 'Test-MemoryCorpusStdin'
    if (@($consumer).Count -ne 1) { throw 'Actual corpus stdin consumer is missing or ambiguous.' }
    foreach ($mutated in @($false, $true)) {
        $text = $consumer.Extent.Text
        if ($mutated) {
            $text = $text.Replace("@('memory-benchmark', `$mode, '--source-stdin', '--source-bytes',", "@('memory-benchmark', '--neutron-star', '--source-stdin', '--source-bytes',")
            if ($text -eq $consumer.Extent.Text) { throw 'Backend negative control no longer reaches the actual plan.' }
        }
        . ([scriptblock]::Create($text))
        foreach ($hip in @($false, $true)) {
            $expectedMode = if ($hip) { '--neutron-star' } else { '--force-cpu' }
            $calls = [Collections.Generic.List[string]]::new()
            # Purpose: Admit only the capability-selected backend and stop after the real consumer's plan check.
            # Inputs: Actual consumer arguments, readback fixture extent and the scoped HIP capability.
            # Outputs: Valid readback/metadata records or an explicit capability failure/rejection-phase sentinel.
            function Invoke-MemoryCorpusProbe {
                param([string[]]$Argument, [byte[]]$InputBytes)
                $calls.Add($Argument[1])
                if ($Argument[1] -ne $expectedMode) { throw 'Corpus plan requested an unavailable backend.' }
                if ($Argument -contains '--plan-only') {
                    return @{ exit_code = 0; stderr = ''; stdout = 'input_bytes=175101388 source_identity_verified=false' }
                }
                if ($calls.Count -gt 2) { throw 'corpus-backend-control-complete' }
                if ($InputBytes.Length -ne 256) { throw 'Actual corpus readback fixture extent changed.' }
                $hash = $Argument[$Argument.IndexOf('--source-sha256') + 1]
                return @{ exit_code = 0; stderr = ''; stdout = "validated_bytes=256 source_sha256=$hash memory_only=true disk_write_bytes=0" }
            }
            $cause = ''
            try { Test-MemoryCorpusStdin -Hip $hip } catch { $cause = $_.Exception.Message }
            $expectedCause = if ($mutated -and -not $hip) { 'Corpus plan requested an unavailable backend.' } else { 'corpus-backend-control-complete' }
            if ($cause -ne $expectedCause -or $calls.Count -lt 2) {
                throw "Actual corpus capability path or its negative control failed: $cause"
            }
        }
    }
}

if ($AllRuntimes) {
    foreach ($runtime in @('powershell', 'pwsh')) {
        $cli = (Get-Command $runtime -ErrorAction Stop).Source
        $result = Invoke-MemoryCorpusProbe -Argument @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        if ($result.exit_code -ne 0 -or $result.stderr) { throw "Binary transport contract failed under ${runtime}: $($result.stderr)" }
        Write-Output $result.stdout.Trim()
    }
    return
}

Test-CorpusPlanningBackend

$cli = (& py -3 -B -c 'import sys; print(sys.executable)')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cli -PathType Leaf)) { throw 'Python is required for the offline binary pipe consumer.' }
$pythonCode = 'import hashlib,sys; b=sys.stdin.buffer.read(); print(len(b),hashlib.sha256(b).hexdigest())'
$previousEncoding = [Console]::InputEncoding
try {
    [Console]::InputEncoding = [Text.UTF8Encoding]::new($true)
    foreach ($bytes in @([byte[]](0..255), [byte[]](0,13,10,26,255), [byte[]]@())) {
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
        finally { $hasher.Dispose() }
        $result = Invoke-MemoryCorpusProbe -Argument @('-B', '-c', $pythonCode) -InputBytes $bytes
        if ($result.exit_code -ne 0 -or $result.stderr -or $result.stdout.Trim() -ne "$($bytes.Length) $hash") {
            throw 'Binary transport changed exact bytes or appended a preamble.'
        }
        if ([Console]::InputEncoding.GetPreamble().Length -eq 0) { throw 'Adapter failed to restore caller encoding.' }
    }
    # The same production owner must restore state when starting its child fails.
    $cli = Join-Path $root ('out/nonexistent-binary-probe-' + [guid]::NewGuid().ToString('N'))
    $rejected = $false
    try { Invoke-MemoryCorpusProbe -Argument @('-B') -InputBytes ([byte[]](1)) | Out-Null } catch { $rejected = $true }
    if (-not $rejected -or [Console]::InputEncoding.GetPreamble().Length -eq 0) { throw 'Failed child start lost encoding restoration.' }

    # Retain a negative control against the actual mechanism, without editing the repository or launching native code.
    $mutant = $original.Replace('$info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)', '$info.StandardInputEncoding = [Text.UTF8Encoding]::new($true)')
    $mutant = $mutant.Replace('[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)', '$null = $false')
    if ($mutant -eq $original) { throw 'Encoding negative control no longer reaches the production mechanism.' }
    . ([scriptblock]::Create($mutant))
    $cli = (& py -3 -B -c 'import sys; print(sys.executable)')
    $rejected = $false
    try { Invoke-MemoryCorpusProbe -Argument @('-B', '-c', $pythonCode) -InputBytes ([byte[]](0..255)) | Out-Null }
    catch { $rejected = $_.Exception.Message -eq 'Binary corpus writer can prepend a text preamble.' }
    if (-not $rejected) { throw 'Preamble-capable encoding escaped the executable recurrence control.' }
} finally {
    . ([scriptblock]::Create($original))
    [Console]::InputEncoding = $previousEncoding
}
Write-Output 'Offline binary corpus contracts passed: exact bytes, ambient preamble, failure restoration, backend selection and negative controls; no product build or benchmark.'
