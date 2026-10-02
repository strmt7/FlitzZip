$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rocm_toolchain.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('superzip-rocm-' + [guid]::NewGuid().ToString('N'))
$manifest = Join-Path $fixture 'share/therock/therock_manifest.json'
$inputs = @('bin/hipcc.exe', 'lib/amdhip64.lib', 'include/hip/hip_version.h',
    'lib/llvm/bin/clang.exe', 'lib/llvm/amdgcn/bitcode/ocml.bc')
$null = New-Item -ItemType Directory -Path $fixture
try {
    foreach ($relative in $inputs + @('share/therock/therock_manifest.json')) {
        $file = Join-Path $fixture $relative
        $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $file))
        [IO.File]::WriteAllText($file, 'fixture; never executed')
    }
    [IO.File]::WriteAllText($manifest, '{"rocm_version":"10.0.0","rocm_package_version":"10.0.0rc4"}')
    if ((Resolve-RocmSdkRoot -RepoRoot $fixture -RequestedPath $fixture) -cne $fixture) {
        throw 'The explicit complete SDK must override discovery.'
    }
    foreach ($version in @('7.2.0', '10.1.0', '', $null)) {
        [IO.File]::WriteAllText($manifest, (@{ rocm_version = $version } | ConvertTo-Json))
        $rejected = $false
        try { Assert-RocmSdkRoot -Root $fixture | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'A different or unavailable SDK version was accepted.' }
    }
    [IO.File]::WriteAllText($manifest, '{"rocm_version":"10.0.0"}')
    foreach ($relative in $inputs) {
        $file = Join-Path $fixture $relative
        [IO.File]::Delete($file)
        $rejected = $false
        try { Assert-RocmSdkRoot -Root $fixture | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'An incomplete SDK was accepted.' }
        [IO.File]::WriteAllText($file, 'fixture; never executed')
    }
    foreach ($path in @('relative/sdk', "$fixture&exit", "$fixture`n", "$fixture%PATH%")) {
        $rejected = $false
        try { Assert-RocmSdkRoot -Root $path | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'An unsafe compiler root was accepted.' }
    }
    $names = @('HIP_PATH', 'ROCM_PATH', 'HIP_PLATFORM', 'HIP_DEVICE_LIB_PATH', 'LLVM_PATH')
    $previous = @{}
    foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    foreach ($failAction in @($false, $true)) {
        $caught = $false
        try {
            $result = @(Invoke-RocmCompilerEnvironment -Root $fixture -Action {
                if ($env:HIP_PATH -cne $fixture -or $env:ROCM_PATH -cne $fixture -or
                    $env:HIP_PLATFORM -cne 'amd' -or
                    $env:HIP_DEVICE_LIB_PATH -cne (Join-Path $fixture 'lib/llvm/amdgcn/bitcode') -or
                    $env:LLVM_PATH -cne (Join-Path $fixture 'lib/llvm')) {
                    throw 'Compiler inputs do not share the selected SDK.'
                }
                if ($failAction) { throw 'test: compiler failure' }
                42
            })
            if ($result.Count -ne 1 -or $result[0] -ne 42) { throw 'Compiler scope contaminated action output.' }
        } catch {
            if ($_.Exception.Message -cne 'test: compiler failure') { throw }
            $caught = $true
        }
        if ($caught -ne $failAction) { throw 'Compiler scope changed failure behavior.' }
        foreach ($name in $names) {
            if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) {
                throw "Compiler scope leaked its $name setting into runtime work."
            }
        }
    }
} finally {
    # This fresh, explicitly named temporary fixture contains no user files.
    $full = [IO.Path]::GetFullPath($fixture)
    $temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a ROCm test fixture outside the intended temporary directory.'
    }
    if (Test-Path -LiteralPath $full) { [IO.Directory]::Delete($full, $true) }
}
$repo = Split-Path -Parent $PSScriptRoot
$tokens = $null
$errors = $null
$policy = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'security_scan.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Security policy did not parse.' }
$definition = $policy.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-RocmProvisioningPolicy'
}, $false)
if ($null -eq $definition) { throw 'ROCm release provisioning policy is missing.' }
. ([scriptblock]::Create($definition.Extent.Text))
$action = [IO.File]::ReadAllText((Join-Path $repo '.github/actions/windows-release/action.yml'))
Test-RocmProvisioningPolicy -ReleaseAction $action
foreach ($invalid in @($action.Replace('python tools/bootstrap_rocm_sdk.py', 'python other_tool.py'),
        ($action + "`nsetx SDK_ROOT ignored`n"),
        ($action + "`nAdd-Content -LiteralPath `$env:GITHUB_ENV -Value 'LLVM_PATH=compiler-only'`n"))) {
    $rejected = $false
    try { Test-RocmProvisioningPolicy -ReleaseAction $invalid } catch { $rejected = $true }
    if (-not $rejected) { throw 'Hosted ROCm provisioning policy accepted an unsafe path.' }
}
Write-Output 'rocm_toolchain status=passed scope_restore=success_and_exception'
