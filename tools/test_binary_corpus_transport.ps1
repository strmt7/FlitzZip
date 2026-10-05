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

if ($AllRuntimes) {
    foreach ($runtime in @('powershell', 'pwsh')) {
        $cli = (Get-Command $runtime -ErrorAction Stop).Source
        $result = Invoke-MemoryCorpusProbe -Argument @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        if ($result.exit_code -ne 0 -or $result.stderr) { throw "Binary transport contract failed under ${runtime}: $($result.stderr)" }
        Write-Output $result.stdout.Trim()
    }
    return
}

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
Write-Output 'Offline binary corpus contracts passed: exact bytes, ambient preamble, failure restoration and negative control; no product build or benchmark.'
