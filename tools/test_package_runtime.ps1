$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_runtime.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('flitzzip-runtime-contract-' + [guid]::NewGuid().ToString('N'))

# Purpose: Require the production boundary to reject one independent invalid payload/state.
# Inputs: Action executes the actual validator. Outputs: Throws if the invalid condition was admitted.
function Assert-RuntimeRejected {
    param([scriptblock]$Action, [string]$ExpectedMessage = '')
    $rejected = $false
    try { & $Action } catch {
        if ($ExpectedMessage -and $_.Exception.Message -notlike "*$ExpectedMessage*") { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Runtime contract admitted an invalid payload or capability state.' }
}

# Purpose: Exercise exact native-tool identity without executing a foreign or damaged binary.
# Inputs: Independent successful and failed probe observations. Outputs: Rejects every ambiguous identity.
function Test-WixVersionAdmission {
    $tool = Get-FlitzZipWixToolchain -RepoRoot $fixture
    Assert-FlitzZipWixVersion -Output @('7.0.1+642f1a0') -ExitCode 0 -Expected $tool.CliVersion
    foreach ($output in @(@('7.0.10'), @('7.0.1-rc.1'), @('7.0.0+642f1a0'), @('7.0.1'),
            @('7.0.1+642f1a0', 'extra'), @(), @('7.0.1+unexpected'))) {
        Assert-RuntimeRejected -ExpectedMessage 'version admission' -Action {
            Assert-FlitzZipWixVersion -Output $output -ExitCode 0 -Expected $tool.CliVersion
        }
    }
    Assert-RuntimeRejected -ExpectedMessage 'version admission' -Action {
        Assert-FlitzZipWixVersion -Output @('7.0.1+642f1a0') -ExitCode 17 -Expected $tool.CliVersion
    }
    Assert-RuntimeRejected { Resolve-FlitzZipWix -RepoRoot $fixture }
}

# Purpose: Reject a missing, invalid or redirected UI assembly before CPack can load it.
# Inputs: Private cache fixtures contain no executable extension. Outputs: Every malformed cache remains rejected.
function Test-WixUiAdmission {
    $tool = Get-FlitzZipWixToolchain -RepoRoot $fixture
    Assert-RuntimeRejected { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    $directory = Join-Path $tool.ExtensionHome ".wix/extensions/WixToolset.UI.wixext/$($tool.Version)/wixext7"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $assembly = Join-Path $directory 'WixToolset.UI.wixext.dll'
    [IO.File]::WriteAllText($assembly, 'invalid extension fixture')
    Assert-RuntimeRejected -ExpectedMessage 'extension identity' -Action { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    Remove-Item -LiteralPath $assembly
    $real = Join-Path $fixture 'real-extension-cache'
    New-Item -ItemType Directory -Path $real | Out-Null
    $link = Join-Path $directory 'WixToolset.UI.wixext.dll'
    New-Item -ItemType Junction -Path $link -Target $real | Out-Null
    try {
        Assert-RuntimeRejected -ExpectedMessage 'redirected' -Action { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    } finally { [IO.Directory]::Delete($link) }
}

# Purpose: Prove packaging restores the caller's discovery state after both normal completion and failure.
# Inputs: A controlled resolver owns no external installation. Outputs: Exact original environment survives both outcomes.
function Test-WixEnvironmentOwnership {
    $savedResolver = ${function:Resolve-FlitzZipWix}
    $before = @($env:PATH, $env:WIX, $env:WIX_EXTENSIONS)
    try {
        function Resolve-FlitzZipWix { param([string]$RepoRoot) return (Get-FlitzZipWixToolchain -RepoRoot $RepoRoot).Path }
        foreach ($fail in @($false, $true)) {
            $packagingAction = {
                param($wix, $extension)
                $expected = Get-FlitzZipWixToolchain -RepoRoot $fixture
                if ($wix -cne $expected.Path -or $extension -cne $expected.UiExtension -or
                    $env:WIX_EXTENSIONS -cne $expected.ExtensionHome -or $env:WIX -cne (Split-Path -Parent $wix) -or
                    -not $env:PATH.StartsWith($env:WIX + [IO.Path]::PathSeparator, [StringComparison]::Ordinal)) {
                    throw 'Packaging did not receive its owned tool environment.'
                }
                if ($fail) { throw 'Independent packaging failure.' }
            }
            if ($fail) {
                Assert-RuntimeRejected -ExpectedMessage 'Independent packaging failure' -Action {
                    Invoke-FlitzZipWixEnvironment -RepoRoot $fixture -Action $packagingAction
                }
            } else { Invoke-FlitzZipWixEnvironment -RepoRoot $fixture -Action $packagingAction }
            if ($env:PATH -cne $before[0] -or $env:WIX -cne $before[1] -or $env:WIX_EXTENSIONS -cne $before[2]) {
                throw 'Packaging changed its caller environment.'
            }
        }
    } finally { ${function:Resolve-FlitzZipWix} = $savedResolver }
}

# Exercise the real process exit boundary and artifact owner together. The child
# is a deterministic external generator; it does not install a protocol fixture.
function Test-PackageGenerationTransaction {
    param([string]$Root)
    $output = Join-Path $Root 'packages'
    New-Item -ItemType Directory -Path $output | Out-Null
    $child = Join-Path $Root 'generator.ps1'
    @'
param([string]$Output, [string]$Behavior)
$ErrorActionPreference = 'Stop'
if ($Behavior -eq 'missing') { exit 0 }
if ($Behavior -eq 'empty') { [IO.File]::WriteAllBytes($Output, [byte[]]@()); exit 0 }
[IO.File]::WriteAllText($Output, 'new complete package')
if ($Behavior -eq 'failed') { exit 17 }
exit 0
'@ | Set-Content -LiteralPath $child
    $shellName = if (Test-Path -LiteralPath (Join-Path $PSHOME 'powershell.exe')) { 'powershell.exe' } else { 'pwsh.exe' }
    $shell = Join-Path $PSHOME $shellName
    $destination = Join-Path $output 'candidate.msi'
    [IO.File]::WriteAllText($destination, 'preceding package')
    foreach ($behavior in @('failed', 'missing', 'empty', 'invalid')) {
        $generate = {
            param($candidate)
            Invoke-FlitzZipPackageTool -FilePath $shell -Operation 'Fixture generator' -Arguments @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $child, '-Output', $candidate, '-Behavior', $behavior)
        }
        $validate = {
            param($candidate)
            if ($behavior -eq 'invalid') { throw 'Independent artifact validation rejected the candidate.' }
            if ([IO.File]::ReadAllText($candidate) -cne 'new complete package') { throw 'Unexpected generated bytes.' }
        }
        $expected = switch ($behavior) {
            'failed' { 'exit code 17' }
            'empty' { 'complete regular artifact' }
            'invalid' { 'validation rejected' }
            default { '' }
        }
        Assert-RuntimeRejected -ExpectedMessage $expected -Action {
            Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
                -Generate $generate -Validate $validate
        }
        if ([IO.File]::ReadAllText($destination) -cne 'preceding package') {
            throw "Failed $behavior generation replaced the preceding artifact."
        }
        if (@(Get-ChildItem -LiteralPath $output -Directory).Count -ne 0) { throw 'Failed generation leaked its workspace.' }
    }
    $behavior = 'success'
    $published = Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
        -Generate $generate -Validate $validate
    if ($published -cne $destination -or [IO.File]::ReadAllText($published) -cne 'new complete package') {
        throw 'Successful generation did not replace the exact preceding artifact.'
    }
    Remove-Item -LiteralPath $destination
    $published = Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
        -Generate $generate -Validate $validate
    if ([IO.File]::ReadAllText($published) -cne 'new complete package') { throw 'First publication failed.' }
    foreach ($name in @('../escaped.msi', 'nested/file.msi', 'C:\escaped.msi', '.')) {
        Assert-RuntimeRejected {
            Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName $name `
                -Generate { throw 'Invalid name reached generation.' }
        }
    }
    if (@(Get-ChildItem -LiteralPath $output -Directory).Count -ne 0) { throw 'Successful generation leaked its workspace.' }
    $occupied = Join-Path $output 'occupied.msi'
    New-Item -ItemType Directory -Path $occupied | Out-Null
    Assert-RuntimeRejected {
        Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'occupied.msi' `
            -Generate $generate -Validate $validate
    }
    if (-not (Test-Path -LiteralPath $occupied -PathType Container)) { throw 'Publication changed an unrelated directory.' }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $fixture 'bin') -Force | Out-Null
    $entries = @()
    foreach ($name in @('libzstd.dll', 'libwim-15.dll', 'flitzzip_hip_kernels.dll')) {
        $file = Join-Path $fixture "bin/$name"
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::UTF8.GetBytes("fixture $name"))
        $entries += @{ name = $name; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $manifest = @{ product = 'FlitzZip'; gpu_backend = @{ enabled = $true }; packaged_runtime_files = $entries }
    $manifestPath = Join-Path $fixture 'flitzzip-runtime-dependencies.json'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true
    $module = Join-Path $fixture 'bin/flitzzip_hip_kernels.dll'
    [IO.File]::AppendAllText($module, 'changed')
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    Remove-Item -LiteralPath $module
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    $manifest.packaged_runtime_files = @($entries[0], $entries[1], $entries[1])
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    foreach ($state in @(
        @{ code = 0; ready = 'true'; runtime = 'true'; kernel = 'true' },
        @{ code = 11; ready = 'false'; runtime = 'false'; kernel = 'false' },
        @{ code = 12; ready = 'false'; runtime = 'true'; kernel = 'false' }
    )) {
        $output = @('hip_compiled=true', "available=$($state.ready)",
            "hip_runtime_loadable=$($state.runtime)", "hip_kernel_loadable=$($state.kernel)")
        Assert-FlitzZipHipDependencyState -Output $output -ExitCode $state.code
        Assert-FlitzZipHipDependencyState -Output ($output + '') -ExitCode $state.code
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output $output -ExitCode 13 }
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output ($output + 'available=false') -ExitCode $state.code }
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output $output[0..2] -ExitCode $state.code }
    }
    Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output @('hip_compiled=false') -ExitCode 10 }
    Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output @('hip_compiled=true', 'available=true') -ExitCode 11 }
    Test-WixVersionAdmission
    Test-WixEnvironmentOwnership
    Test-WixUiAdmission
    Test-PackageGenerationTransaction -Root $fixture
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
