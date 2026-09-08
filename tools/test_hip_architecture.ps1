param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "hip_architecture.ps1")

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

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "compile_hip_object.ps1"), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "HIP compiler wrapper did not parse." }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-HipCompile'
}, $false)
if ($null -eq $definition) { throw "HIP compiler invocation function is missing." }
. ([scriptblock]::Create($definition.Extent.Text))

# Purpose: Simulate compiler diagnostics and native status without invoking a toolchain.
# Inputs: Arguments are deliberately ignored; CompilerExitCode supplies the native result.
# Outputs: Emits a diagnostic and sets the simulated native exit code.
function cmd {
    $global:LASTEXITCODE = $script:CompilerExitCode
    'simulated compiler diagnostic'
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("superzip-hip-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temporary
$script:Arch = 'release'
$script:Source = 'unused-source'
$script:Output = Join-Path $temporary 'not-created.obj'
$savedExitCode = $global:LASTEXITCODE
try {
    foreach ($script:CompilerExitCode in @(0, 7)) {
        $result = @(Invoke-HipCompile -VcvarsAll 'unused-vcvars' -CandidateVersion '14.44' `
            -HipccPath 'unused-hipcc' -IncludePath 'unused-include' 6>$null)
        if ($result.Count -ne 1 -or $result[0] -isnot [int] -or $result[0] -ne $script:CompilerExitCode) {
            throw "Compiler diagnostics contaminated the returned native exit code."
        }
    }
} finally {
    $global:LASTEXITCODE = $savedExitCode
    [IO.Directory]::Delete($temporary)
}

Write-Output "hip_architecture status=passed valid_cases=$($cases.Count) invalid_cases=$($invalid.Count) compiler_status_cases=2"
