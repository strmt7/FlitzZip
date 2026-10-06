param([ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_runtime.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$buildBin = Join-Path $repo "build/$Configuration"
$fixture = Join-Path $repo ('out/hip-kernel-smoke-' + [guid]::NewGuid().ToString('N'))
$inputBytes = [byte[]](0..255)
$sha = [Security.Cryptography.SHA256]::Create()
try { $inputDigest = ([BitConverter]::ToString($sha.ComputeHash($inputBytes))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
$sourceArguments = "--source-stdin --source-bytes $($inputBytes.Length) --source-sha256 $inputDigest"

# Purpose: Execute one owned CLI probe with finite lifetime and asynchronous output drainage.
# Inputs: File/Arguments select the qualified binary; optional InputBytes is a tiny RAM-only correctness payload.
# Outputs: Returns exit/text; kills only this child on timeout and rejects excessive output.
function Invoke-KernelBoundaryProbe {
    param([string]$File, [string]$Arguments, [byte[]]$InputBytes)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $null -ne $InputBytes
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if ($null -ne $InputBytes) {
            $process.StandardInput.BaseStream.Write($InputBytes, 0, $InputBytes.Length)
            $process.StandardInput.Close()
        }
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            throw 'Owned kernel-boundary probe exceeded its lifetime.'
        }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($text.Length -gt 65536) { throw 'Kernel-boundary output exceeded its fixed diagnostic budget.' }
        return @{ code = $process.ExitCode; text = $text }
    } finally { $process.Dispose() }
}

# Purpose: Require a byte-exact CPU readback while the optional kernel module is absent or rejected.
# Inputs: CLI is the isolated qualified binary. Outputs: Throws on startup failure, disk writes or failed readback.
function Assert-KernelIndependentCpu {
    param([string]$Cli)
    $result = Invoke-KernelBoundaryProbe $Cli "memory-benchmark $sourceArguments --force-cpu --compression-level 1" $inputBytes
    if ($result.code -ne 0 -or $result.text -notmatch 'memory_only=true' -or
        $result.text -notmatch 'disk_write_bytes=0') { throw "CPU independence probe failed: $($result.text)" }
}

& py -3 -B -m tools.native_build_receipt validate --configuration $Configuration --require-hip *> (Join-Path $repo 'out/hip-kernel-smoke-native-identity.json')
if ($LASTEXITCODE) { throw 'Kernel runtime smoke requires a current HIP-enabled native receipt.' }
$names = @('superzip_cli.exe', 'libzstd.dll', 'libwim-15.dll', 'superzip_hip_kernels.dll')
$bytes = ($names | ForEach-Object { (Get-Item -LiteralPath (Join-Path $buildBin $_)).Length } | Measure-Object -Sum).Sum
if ($bytes + (Get-Item -LiteralPath (Join-Path $buildBin 'superzip_hip_kernels.dll')).Length -gt 64MB) {
    throw 'Kernel boundary filesystem fixture exceeds 64 MiB.'
}
try {
    $bin = Join-Path $fixture 'bin'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $buildBin $name) -Destination $bin }
    Copy-Item -LiteralPath (Join-Path $repo 'build/superzip-runtime-dependencies.json') -Destination $fixture
    $cli = Join-Path $bin 'superzip_cli.exe'
    Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true
    $valid = Invoke-KernelBoundaryProbe $cli 'dependency-check'
    Assert-SuperZipHipDependencyState -Output ($valid.text -split '\r?\n') -ExitCode $valid.code
    Assert-KernelIndependentCpu $cli
    $module = Join-Path $bin 'superzip_hip_kernels.dll'
    foreach ($case in @('missing', 'modified')) {
        if ($case -eq 'missing') { Remove-Item -LiteralPath $module }
        else {
            Copy-Item -LiteralPath (Join-Path $buildBin 'superzip_hip_kernels.dll') -Destination $module
            $stream = [IO.File]::Open($module, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            try { $stream.SetLength($stream.Length - 1) }
            finally { $stream.Dispose() }
        }
        $rejected = $false
        try { Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true } catch { $rejected = $true }
        if (-not $rejected) { throw "Package integrity admitted the $case kernel module." }
        $state = Invoke-KernelBoundaryProbe $cli 'dependency-check'
        $expected = if ($valid.code -eq 0) { 13 } else { $valid.code }
        if ($state.code -ne $expected) { throw "Unexpected $case module state: $($state.text)" }
        Assert-KernelIndependentCpu $cli
        $neutron = Invoke-KernelBoundaryProbe $cli "memory-benchmark $sourceArguments --require-gpu --neutron-star" $inputBytes
        if ($neutron.code -eq 0 -or $neutron.text -notmatch 'AMD HIP|HIP runtime') {
            throw "Required-HIP Neutron failed to reject the $case kernel module at device admission: $($neutron.text)"
        }
        Write-Output "Kernel boundary ${case}: integrity rejected, CPU usable, required-HIP rejected."
    }
    Write-Output "Kernel boundary qualified; baseline HIP state=$($valid.code). Hardware absence is not GPU execution evidence."
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $fixture).Path)
        if ($resolved -cne [IO.Path]::GetFullPath($fixture) -or
            (Get-Item -LiteralPath $fixture -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing redirected kernel fixture cleanup.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
