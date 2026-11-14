param([ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_runtime.ps1')
. (Join-Path $PSScriptRoot 'native_process_arguments.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$buildBin = Join-Path $repo "build/$Configuration"
$fixture = Join-Path $repo ('out/hip-kernel-smoke-' + [guid]::NewGuid().ToString('N'))
$inputBytes = [byte[]](0..255)
$sha = [Security.Cryptography.SHA256]::Create()
try { $inputDigest = ([BitConverter]::ToString($sha.ComputeHash($inputBytes))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
$maxCpuArchiveBytes = 4096

# Purpose: Execute one owned CLI probe with finite lifetime and asynchronous output drainage.
# Inputs: File selects the qualified binary; Command permits only ordinary capability/archive operations.
# Arguments are exact native arguments. This probe cannot invoke a measurement controller.
# Outputs: Returns exit/text; kills only this child on timeout and rejects excessive output.
function Invoke-KernelBoundaryProbe {
    param(
        [string]$File,
        [ValidateSet('dependency-check', 'compress', 'verify', 'extract')][string]$Command,
        [string[]]$Arguments = @()
    )
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $commandArguments = @($Command) + @($Arguments)
    $info.Arguments = ($commandArguments | ForEach-Object { ConvertTo-FlitzZipWindowsArgument -Value $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw 'Owned kernel-boundary probe exceeded its lifetime.'
        }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($text.Length -gt 65536) { throw 'Kernel-boundary output exceeded its fixed diagnostic budget.' }
        return @{ code = $process.ExitCode; text = $text }
    } finally {
        try {
            if (-not $process.HasExited) {
                $process.Kill()
                [void]$process.WaitForExit(5000)
            }
        } finally { $process.Dispose() }
    }
}

# Purpose: Require a byte-exact CPU readback while the optional kernel module is absent or rejected.
# Inputs: CLI is the isolated qualified binary; InputPath is the tiny fixed fixture and WorkRoot is fresh owned storage.
# Outputs: Requires ordinary compression, verification and independent SHA-256 readback within the fixture budget.
function Assert-KernelIndependentCpu {
    param([string]$Cli, [string]$InputPath, [string]$WorkRoot)
    New-Item -ItemType Directory -Path $WorkRoot | Out-Null
    $archive = Join-Path $WorkRoot 'cpu archive.flzip'
    $restored = Join-Path $WorkRoot 'restored output'
    $create = Invoke-KernelBoundaryProbe -File $Cli -Command compress -Arguments @(
        '--format', 'flzip', '--force-cpu', '--compression-level', '1', '--output', $archive, $InputPath)
    if ($create.code -ne 0) { throw "CPU compression probe failed: $($create.text)" }
    if ((Get-Item -LiteralPath $archive).Length -gt $maxCpuArchiveBytes) {
        throw 'CPU correctness archive exceeded its fixed fixture budget.'
    }
    $verify = Invoke-KernelBoundaryProbe -File $Cli -Command verify -Arguments @('--force-cpu', $archive)
    if ($verify.code -ne 0) { throw "CPU verification probe failed: $($verify.text)" }
    $extract = Invoke-KernelBoundaryProbe -File $Cli -Command extract -Arguments @(
        '--force-cpu', '--output', $restored, $archive)
    if ($extract.code -ne 0) { throw "CPU extraction probe failed: $($extract.text)" }
    $target = Join-Path $restored ([IO.Path]::GetFileName($InputPath))
    if ((Get-Item -LiteralPath $target).Length -ne $inputBytes.Length -or
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -cne $inputDigest) {
        throw 'CPU correctness readback differs from the original independent fixture.'
    }
}

& py -3 -B -m tools.native_build_receipt validate --configuration $Configuration --require-hip *> (Join-Path $repo 'out/hip-kernel-smoke-native-identity.json')
if ($LASTEXITCODE) { throw 'Kernel runtime smoke requires a current HIP-enabled native receipt.' }
$names = @('flitzzip_cli.exe', 'libzstd.dll', 'libwim-15.dll', 'flitzzip_hip_kernels.dll')
$bytes = ($names | ForEach-Object { (Get-Item -LiteralPath (Join-Path $buildBin $_)).Length } | Measure-Object -Sum).Sum
$roundtripBytes = 3 * ($maxCpuArchiveBytes + $inputBytes.Length) + $inputBytes.Length
if ($bytes + (Get-Item -LiteralPath (Join-Path $buildBin 'flitzzip_hip_kernels.dll')).Length + $roundtripBytes -gt 64MB) {
    throw 'Kernel boundary filesystem fixture exceeds 64 MiB.'
}
try {
    $bin = Join-Path $fixture 'bin'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $buildBin $name) -Destination $bin }
    Copy-Item -LiteralPath (Join-Path $buildBin 'flitzzip-runtime-dependencies.json') -Destination $fixture
    $cli = Join-Path $bin 'flitzzip_cli.exe'
    $sourceFile = Join-Path $fixture 'input payload.bin'
    [IO.File]::WriteAllBytes($sourceFile, $inputBytes)
    Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true
    $valid = Invoke-KernelBoundaryProbe -File $cli -Command dependency-check
    Assert-FlitzZipHipDependencyState -Output ($valid.text -split '\r?\n') -ExitCode $valid.code
    Assert-KernelIndependentCpu -Cli $cli -InputPath $sourceFile -WorkRoot (Join-Path $fixture 'cpu-valid')
    $module = Join-Path $bin 'flitzzip_hip_kernels.dll'
    foreach ($case in @('missing', 'modified')) {
        if ($case -eq 'missing') { Remove-Item -LiteralPath $module }
        else {
            Copy-Item -LiteralPath (Join-Path $buildBin 'flitzzip_hip_kernels.dll') -Destination $module
            $stream = [IO.File]::Open($module, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            try { $stream.SetLength($stream.Length - 1) }
            finally { $stream.Dispose() }
        }
        $rejected = $false
        try { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true } catch { $rejected = $true }
        if (-not $rejected) { throw "Package integrity admitted the $case kernel module." }
        $state = Invoke-KernelBoundaryProbe -File $cli -Command dependency-check
        $expected = if ($valid.code -eq 0) { 13 } else { $valid.code }
        if ($state.code -ne $expected) { throw "Unexpected $case module state: $($state.text)" }
        Assert-KernelIndependentCpu -Cli $cli -InputPath $sourceFile -WorkRoot (Join-Path $fixture "cpu-$case")
        $neutronArchive = Join-Path $fixture "neutron-$case.flzip"
        $neutron = Invoke-KernelBoundaryProbe -File $cli -Command compress -Arguments @(
            '--format', 'flzip', '--require-gpu', '--neutron-star', '--output', $neutronArchive, $sourceFile)
        if ($neutron.code -eq 0 -or $neutron.text -notmatch 'AMD HIP|HIP runtime') {
            throw "Required-HIP Neutron failed to reject the $case kernel module at device admission: $($neutron.text)"
        }
        if (Test-Path -LiteralPath $neutronArchive) { throw 'Rejected Neutron operation published an archive.' }
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
