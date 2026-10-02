param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "hip_architecture.ps1")
. (Join-Path $PSScriptRoot "cmake_toolchain.ps1")

$releaseTargets = "gfx1100,gfx1101,gfx1102,gfx1151,gfx1200,gfx1201"
$cases = @(
    @{ Input = "gfx1201"; Expected = "gfx1201" },
    @{ Input = "gfx1100,gfx1201"; Expected = "gfx1100,gfx1201" },
    @{ Input = "release"; Expected = $releaseTargets },
    @{ Input = ((1..16 | ForEach-Object { "gfx$_" }) -join ',');
       Expected = ((1..16 | ForEach-Object { "gfx$_" }) -join ',') }
)
foreach ($case in $cases) {
    $actual = Resolve-HipArchitecture -Architecture $case.Input
    if ($actual -cne $case.Expected) {
        throw "HIP architecture resolution changed for $($case.Input)."
    }
    $expectedArguments = ($case.Expected.Split(',') | ForEach-Object { "--offload-arch=$_" }) -join ' '
    if ((Get-HipOffloadArgument -Architecture $case.Input) -cne $expectedArguments) {
        throw "HIP compiler arguments dropped or changed a requested target."
    }
}

$invalid = @("", "release,gfx1201", "RELEASE", "GFX1201", " gfx1201", "gfx1201 ",
    "gfx1201,", ",gfx1201", "gfx1201,,gfx1100", "gfx1201,gfx1201", "gfx1201;gfx1100",
    "gfx1201 --version", "gfx1201&exit", "gfx1201`n", "gfx1201`r`n", "gfx1201`"", "native",
    ("gfx" + ('1' * 254)), ((1..17 | ForEach-Object { "gfx$_" }) -join ','))
foreach ($value in $invalid) {
    $rejected = $false
    try {
        $null = Get-HipOffloadArgument -Architecture $value
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw "An invalid HIP architecture selection was accepted."
    }
}

# The configure path must use the same resolver, including rejected selections.
$repoRoot = Split-Path -Parent $PSScriptRoot
$cmake = Find-CMake -RepoRoot $repoRoot
foreach ($selection in @('release', 'gfx1100,gfx1201', 'gfx1201,gfx1201', 'native', 'gfx1201;gfx1100')) {
    $expected = if ($selection -eq 'release') { $releaseTargets } else { $selection }
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $cmake "-DREPO_ROOT=$repoRoot" "-DSELECTION=$selection" "-DEXPECTED=$expected" `
            -P (Join-Path $repoRoot 'tests/cmake/test_hip_architecture.cmake') 2>&1)
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    $shouldPass = $selection -in @('release', 'gfx1100,gfx1201')
    if (($exitCode -eq 0) -ne $shouldPass) {
        throw "Configure-time architecture boundary diverged: $selection; $($output -join ' ')"
    }
}
foreach ($scriptCase in @(@{ Name = 'build.ps1'; Parameter = 'HipArch' },
                         @{ Name = 'compile_hip_object.ps1'; Parameter = 'Arch' })) {
    $defaultTokens = $null
    $defaultErrors = $null
    $defaultAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot $scriptCase.Name), [ref]$defaultTokens, [ref]$defaultErrors)
    $defaultParameter = @($defaultAst.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -eq $scriptCase.Parameter
    })
    if ($defaultErrors.Count -ne 0 -or $defaultParameter.Count -ne 1 -or
        $defaultParameter[0].DefaultValue.SafeGetValue() -cne 'release') {
        throw 'Ordinary build and device compilation must default to the portable release preset.'
    }
}

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "compile_hip_object.ps1"), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "HIP compiler wrapper did not parse." }
foreach ($functionName in @('Invoke-HipCompile', 'Add-HipDeviceDependency', 'ConvertTo-HipDependencyPath', 'Invoke-HipCompileCleanup')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
    }, $false)
    if ($null -eq $definition) { throw "HIP compiler function is missing: $functionName" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
if ((ConvertTo-HipDependencyPath -Path 'C:\with # and $ signs\header.hpp') -cne 'C:/with\ \#\ and\ $$\ signs/header.hpp') {
    throw 'Dependency escaping must preserve Windows paths containing Make-significant characters.'
}

# Purpose: Simulate compiler diagnostics and native status without invoking a toolchain.
# Inputs: Arguments contain the generated compiler command; CompilerExitCode and DependencyMode select the result.
# Outputs: Records the command, optionally writes fresh dependency/object data, emits diagnostics and sets native status.
function cmd {
    $script:CompilerCommand = $args -join ' '
    if ($script:CompilerCommand.Contains('--offload-device-only')) {
        $script:DeviceCommands += $script:CompilerCommand
        if ($script:DependencyMode -eq 'device-throws') { throw 'simulated native failure' }
        $global:LASTEXITCODE = if ($script:DependencyMode -eq 'device-failed') { 7 } else { 0 }
        '# 1 "C:\\fixture device header.hpp" 1'
        '# 2 "C:\\fixture device header.hpp" 2'
        '# 1 "<built-in>"'
        return
    }
    $script:CompileCommand = $script:CompilerCommand
    if ($script:DependencyFile) {
        if (Test-Path -LiteralPath $script:DependencyFile) { throw 'Stale dependency file reached the compiler.' }
        [IO.File]::WriteAllText($script:Output, 'simulated object')
        if ($script:DependencyMode -in @('fresh', 'device-failed', 'throws', 'device-throws')) {
            [IO.File]::WriteAllText($script:DependencyFile, 'simulated.obj: transitive.hpp')
        } elseif ($script:DependencyMode -eq 'empty') {
            [IO.File]::WriteAllText($script:DependencyFile, '')
        }
        if ($script:DependencyMode -eq 'throws') { throw 'simulated native failure' }
    }
    $global:LASTEXITCODE = $script:CompilerExitCode
    'simulated compiler diagnostic'
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("superzip-hip-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temporary
$script:Arch = 'release'
$script:Source = 'unused-source'
$script:Output = Join-Path $temporary 'not-created.obj'
$script:DependencyFile = ''
$savedExitCode = $global:LASTEXITCODE
try {
    foreach ($script:CompilerExitCode in @(0, 7)) {
        $result = @(Invoke-HipCompile -VcvarsAll 'unused-vcvars' -CandidateVersion '14.44' `
            -HipccPath 'unused-hipcc' -IncludePath 'unused-include' 6>$null)
        if ($result.Count -ne 1 -or $result[0] -isnot [int] -or $result[0] -ne $script:CompilerExitCode) {
            throw "Compiler diagnostics contaminated the returned native exit code."
        }
    }
    $script:Arch = Resolve-HipArchitecture -Architecture 'release'
    $script:DependencyFile = Join-Path $temporary 'dependency with spaces.d'
    foreach ($script:DependencyMode in @('fresh', 'empty', 'missing', 'failed', 'device-failed', 'throws', 'device-throws')) {
        $script:DeviceCommands = @()
        [IO.File]::WriteAllText($script:DependencyFile, 'stale dependency data')
        $script:CompilerExitCode = if ($script:DependencyMode -eq 'failed') { 7 } else { 0 }
        $rejected = $false
        try {
            $result = @(Invoke-HipCompile -VcvarsAll 'unused-vcvars' -CandidateVersion '14.44' `
                -HipccPath 'unused-hipcc' -IncludePath 'unused-include' 6>$null)
        } catch {
            if ($_.Exception.Message -notin @('HIP compiler succeeded without a nonempty requested dependency file.', 'simulated native failure')) { throw }
            $rejected = $true
        }
        if ($rejected -ne ($script:DependencyMode -in @('empty', 'missing', 'throws', 'device-throws'))) {
            throw 'Missing dependency data must reject successful compilation.'
        }
        if ($rejected -and (Test-Path -LiteralPath $script:DependencyFile)) {
            throw 'Exceptional compilation must discard dependency data with the failed object.'
        }
        if ($script:DependencyMode -eq 'fresh') {
            if ($result.Count -ne 1 -or $result[0] -ne 0 -or
                -not [IO.File]::ReadAllText($script:DependencyFile).StartsWith('simulated.obj: transitive.hpp') -or
                -not [IO.File]::ReadAllText($script:DependencyFile).Contains('C:/fixture\ device\ header.hpp')) {
                throw 'Successful compilation must replace stale dependencies with fresh data.'
            }
            if ($script:DeviceCommands.Count -ne $releaseTargets.Split(',').Count) {
                throw 'Dependency scanning must include every requested GPU architecture.'
            }
            foreach ($target in $releaseTargets.Split(',')) {
                if (@($script:DeviceCommands | Where-Object { $_.Contains("--offload-arch=$target ") }).Count -ne 1) {
                    throw 'Each requested GPU architecture must have one dependency pass.'
                }
            }
            $common = '-std=c++20 -O3 -fms-runtime-lib=static -DSUPERZIP_ENABLE_HIP=1 -I"unused-include"'
            foreach ($command in @($script:CompileCommand) + $script:DeviceCommands) {
                if (-not $command.Contains($common)) { throw 'Host and device dependency passes must share compiler policy.' }
            }
        } elseif (Test-Path -LiteralPath $script:Output) {
            throw 'Failed compilation must not leave an object that can appear current.'
        }
        if ($script:DependencyMode -in @('failed', 'device-failed') -and
            ($result.Count -ne 1 -or $result[0] -ne 7 -or (Test-Path -LiteralPath $script:DependencyFile))) {
            throw 'Failed compilation must preserve status and discard stale dependency data.'
        }
        if (-not $script:CompileCommand.Contains('-MD -MF "' + $script:DependencyFile + '" -MQ "' + $script:Output + '"')) {
            throw 'Compiler dependency paths and target must be quoted without dropping spaces.'
        }
    }
} finally {
    $global:LASTEXITCODE = $savedExitCode
    foreach ($file in @($script:Output, $script:DependencyFile)) {
        if ($file -and (Test-Path -LiteralPath $file)) { Remove-Item -LiteralPath $file -Force }
    }
    [IO.Directory]::Delete($temporary)
}

Write-Output "hip_architecture status=passed valid_cases=$($cases.Count) invalid_cases=$($invalid.Count) compiler_status_cases=2 dependency_cases=7"
