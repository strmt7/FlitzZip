$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_runtime.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('superzip-runtime-contract-' + [guid]::NewGuid().ToString('N'))

# Purpose: Require the production boundary to reject one independent invalid payload/state.
# Inputs: Action executes the actual validator. Outputs: Throws if the invalid condition was admitted.
function Assert-RuntimeRejected {
    param([scriptblock]$Action)
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    if (-not $rejected) { throw 'Runtime contract admitted an invalid payload or capability state.' }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $fixture 'bin') -Force | Out-Null
    $entries = @()
    foreach ($name in @('libzstd.dll', 'libwim-15.dll', 'superzip_hip_kernels.dll')) {
        $file = Join-Path $fixture "bin/$name"
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::UTF8.GetBytes("fixture $name"))
        $entries += @{ name = $name; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $manifest = @{ product = 'SuperZip'; gpu_backend = @{ enabled = $true }; packaged_runtime_files = $entries }
    $manifestPath = Join-Path $fixture 'superzip-runtime-dependencies.json'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true
    $module = Join-Path $fixture 'bin/superzip_hip_kernels.dll'
    [IO.File]::AppendAllText($module, 'changed')
    Assert-RuntimeRejected { Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    Remove-Item -LiteralPath $module
    Assert-RuntimeRejected { Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    $manifest.packaged_runtime_files = @($entries[0], $entries[1], $entries[1])
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-RuntimeRejected { Assert-SuperZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    foreach ($state in @(
        @{ code = 0; ready = 'true'; runtime = 'true'; kernel = 'true' },
        @{ code = 11; ready = 'false'; runtime = 'false'; kernel = 'false' },
        @{ code = 12; ready = 'false'; runtime = 'true'; kernel = 'false' }
    )) {
        $output = @('hip_compiled=true', "available=$($state.ready)",
            "hip_runtime_loadable=$($state.runtime)", "hip_kernel_loadable=$($state.kernel)")
        Assert-SuperZipHipDependencyState -Output $output -ExitCode $state.code
        Assert-SuperZipHipDependencyState -Output ($output + '') -ExitCode $state.code
        Assert-RuntimeRejected { Assert-SuperZipHipDependencyState -Output $output -ExitCode 13 }
        Assert-RuntimeRejected { Assert-SuperZipHipDependencyState -Output ($output + 'available=false') -ExitCode $state.code }
        Assert-RuntimeRejected { Assert-SuperZipHipDependencyState -Output $output[0..2] -ExitCode $state.code }
    }
    Assert-RuntimeRejected { Assert-SuperZipHipDependencyState -Output @('hip_compiled=false') -ExitCode 10 }
    Assert-RuntimeRejected { Assert-SuperZipHipDependencyState -Output @('hip_compiled=true', 'available=true') -ExitCode 11 }
    Write-Output 'Package integrity and truthful HIP capability contracts passed.'
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $fixture).Path)
        if ($resolved -cne [IO.Path]::GetFullPath($fixture) -or
            (Get-Item -LiteralPath $fixture -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing redirected runtime fixture cleanup.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
