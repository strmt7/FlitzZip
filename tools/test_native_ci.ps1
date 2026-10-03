$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'superzip_verification.psm1') -Force

# Purpose: Reject missing hosted routing, policy or backend qualification evidence.
# Inputs: Condition describes an expected invariant; Message identifies the affected boundary.
# Outputs: Throws on a false assertion.
function Assert-NativeCi {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Native CI contract failed: $Message" }
}

# Purpose: Apply the actual owned GitHub push/PR filters, including ordered exclusions.
# Inputs: Workflow and Path are checkout-relative fixtures with literal/star path syntax.
# Outputs: Returns filter admission; rejects unsupported patterns or divergent push/PR aliases.
function Test-NativeCiWorkflowPath {
    param([string]$Workflow, [string]$Path)
    $root = Split-Path -Parent $PSScriptRoot
    $text = Get-Content -LiteralPath (Join-Path $root ".github/workflows/$Workflow.yml") -Raw
    $filter = [regex]::Match($text, '(?m)^    paths: &(?<anchor>[\w-]+)\r?\n(?<paths>(?:      - [^\r\n]+\r?\n)+)')
    Assert-NativeCi ($filter.Success -and $text.Contains("    paths: *$($filter.Groups['anchor'].Value)")) "shared event path filter: $Workflow"
    $admitted = $false
    foreach ($line in ($filter.Groups['paths'].Value -split '\r?\n')) {
        if (-not $line) { continue }
        $glob = $line.Substring(8).Trim("'")
        $negative = $glob.StartsWith('!')
        if ($negative) { $glob = $glob.Substring(1) }
        Assert-NativeCi ($glob -notmatch '[!\[\]?]') "unsupported fixture path syntax: $Workflow"
        $pattern = [regex]::Escape($glob).Replace('\*\*', 'DOUBLE_STAR').Replace('\*', '[^/]*').Replace('DOUBLE_STAR', '.*')
        if ($Path -cmatch "^$pattern`$") { $admitted = -not $negative }
    }
    return $admitted
}

foreach ($path in @('src/gpu/hip_codec.hip.cpp', 'src/gpu/hip_codec_static_prefix.hip.cpp',
        'src/gpu/hip_codec_adaptive_prefix.hip.cpp', 'src/gpu/hip_dictionary_matcher.hip.cpp', 'src/gpu/hip_sparse_pattern.hip.cpp')) {
    $selection = Get-SuperZipHostedWorkflowSelection -Paths @($path, 'docs/design.md')
    Assert-NativeCi (-not $selection.windows -and $selection.rocm) "HIP implementation requires actual HIP compilation: $path"
}
foreach ($path in @('CMakeLists.txt', 'cmake/ResolveHipArchitecture.cmake', 'src/core/checksum.hpp',
        'src/gpu/hip_device.cpp', 'src/gpu/hip_codec_support.hpp', 'tools/build.ps1',
        'tools/rocm-sdk-lock.json', 'tools/compile_hip_object.ps1', 'tools/native_build_receipt.py',
        'third_party/lzma_sdk/CpuArch.c')) {
    $selection = Get-SuperZipHostedWorkflowSelection -Paths @($path)
    Assert-NativeCi ($selection.windows -and $selection.rocm) "shared/native ABI inputs retain both builds: $path"
}
foreach ($path in @('src/core/checksum.cpp', 'src/app/main_window_controls.cpp', 'tests/cpp/test_zip_compat.cpp',
        'tools/gui_smoke.ps1', 'resources/design/fixture.svg', 'LICENSE')) {
    $selection = Get-SuperZipHostedWorkflowSelection -Paths @($path)
    Assert-NativeCi ($selection.windows -and -not $selection.rocm) "CPU mechanisms avoid unrelated SDK provisioning: $path"
}
foreach ($path in @('docs/design.md', 'tools/native_test_selection.ps1', 'tools/native_ci.ps1')) {
    $selection = Get-SuperZipHostedWorkflowSelection -Paths @($path)
    Assert-NativeCi (-not $selection.windows -and -not $selection.rocm) "tool/docs contracts do not require product builds: $path"
}
$mixed = Get-SuperZipHostedWorkflowSelection -Paths @('src/gpu/hip_codec.hip.cpp', 'src/core/checksum.cpp')
Assert-NativeCi ($mixed.windows -and $mixed.rocm) 'mixed CPU/HIP batches retain both compile boundaries'
$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$hipSources = @([regex]::Matches($cmake, 'src/gpu/[\w_]+\.hip\.cpp') | ForEach-Object Value | Sort-Object -Unique)
Assert-NativeCi ($hipSources.Count -gt 0) 'CMake must expose HIP translation unit inputs'
$paths = @($hipSources) + @('src/core/checksum.cpp', 'src/core/checksum.hpp', 'src/gpu/hip_codec_support.hpp',
    'src/gpu/nested/future.hip.cpp', 'tests/cpp/test_zip_compat.cpp', 'CMakeLists.txt',
    'tools/compile_hip_object.ps1', 'tools/process_environment.ps1', 'tools/build.ps1',
    'tools/native_build_provenance.py', 'tools/native_build_receipt.py', 'tools/bootstrap_rocm_sdk.py',
    'tools/rocm-sdk-lock.json', 'tools/native_ci.ps1', 'tools/gui_smoke.ps1', 'resources/design/fixture.svg',
    'docs/design.md', '.github/workflows/windows-ci.yml', '.github/workflows/rocm-qualification.yml')
foreach ($path in $paths) {
    $selection = Get-SuperZipHostedWorkflowSelection -Paths @($path)
    Assert-NativeCi ((Test-NativeCiWorkflowPath windows-ci $path) -eq $selection.windows) "CPU planner/filter parity: $path"
    Assert-NativeCi ((Test-NativeCiWorkflowPath rocm-qualification $path) -eq $selection.rocm) "HIP planner/filter parity: $path"
}
foreach ($case in @(
    @{ path = 'tests/cpp/test_dictionary_block.cpp'; build = $true; mode = 'component'; matrix = $false },
    @{ path = 'tests/cpp/test_sparse_pattern_block.cpp'; build = $true; mode = 'component'; matrix = $false },
    @{ path = 'tests/cpp/test_zip_compat.cpp'; build = $true; mode = 'component'; matrix = $false },
    @{ path = 'src/zip/zip_adapter.cpp'; build = $true; mode = 'full'; matrix = $true },
    @{ path = 'tests/cpp/test_main.cpp'; build = $true; mode = 'none'; matrix = $false },
    @{ path = '.github/workflows/windows-ci.yml'; build = $false; mode = 'none'; matrix = $false },
    @{ path = 'docs/design.md'; build = $false; mode = 'none'; matrix = $false }
)) {
    $plan = Get-SuperZipHostedNativePlan -Paths @($case.path)
    Assert-NativeCi ($plan.build -eq $case.build -and $plan.testMode -eq $case.mode -and
        $plan.formatMatrix -eq $case.matrix -and -not $plan.gpuExecutionQualified) "canonical native projection: $($case.path)"
}
$full = Get-SuperZipHostedNativePlan -Paths @('README.md') -Full
Assert-NativeCi ($full.build -and $full.testMode -eq 'full' -and $full.formatMatrix -and $full.securityPolicy) 'explicit broad/manual qualification remains complete'
$manual = Get-SuperZipNativeCiEvent -EventName workflow_dispatch
Assert-NativeCi $manual.full 'manual dispatch is explicit broad qualification'
$initial = Get-SuperZipNativeCiEvent -EventName push -PushBase ('0' * 40)
Assert-NativeCi ('CMakeLists.txt' -in $initial.paths -and -not $initial.full) 'initial pushes classify all tracked inputs'
$base = git rev-parse HEAD~1
if ($LASTEXITCODE -ne 0) { throw 'Event contract requires the checked-out comparison history.' }
$push = Get-SuperZipNativeCiEvent -EventName push -PushBase $base
$pr = Get-SuperZipNativeCiEvent -EventName pull_request -PullRequestBase $base
Assert-NativeCi (($push.paths -join '|') -eq ($pr.paths -join '|')) 'push/PR use the complete authoritative range'
foreach ($invalid in @('', 'HEAD~1', 'invalid', ('0' * 40))) {
    $rejected = $false
    try { Get-SuperZipNativeCiEvent -EventName pull_request -PullRequestBase $invalid | Out-Null } catch { $rejected = $true }
    Assert-NativeCi $rejected 'missing/malformed event bases must fail instead of testing a guessed range'
}
$workflow = Get-Content -LiteralPath (Join-Path $root '.github/workflows/windows-ci.yml') -Raw
foreach ($output in @('native_build', 'native_tests', 'format_matrix', 'policy_scan')) {
    Assert-NativeCi ($workflow.Contains("steps.native_plan.outputs.$output == 'true'")) "actual workflow honors $output admission"
}
Assert-NativeCi ($workflow.Contains('fetch-depth: 0')) 'the event base must be available'
Assert-NativeCi ($workflow.Contains('tools/security_scan.ps1') -and $workflow.Contains("matrix.os == 'windows-2022'")) 'selected policy checks remain without duplicating compiler-independent scans'
$hipWorkflow = Get-Content -LiteralPath (Join-Path $root '.github/workflows/rocm-qualification.yml') -Raw
Assert-NativeCi ($hipWorkflow.Contains('--require-hip') -and $hipWorkflow.Contains("inputs.runner || 'windows-2025-vs2026'")) 'automatic HIP compilation uses an explicit runner and actual required-HIP receipt'
$entry = Join-Path $PSScriptRoot 'ci_native_plan.ps1'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('superzip-native-ci-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$outputPath = Join-Path $fixtureRoot 'outputs.txt'
try {
    $metadata = (& $entry -ChangedPath 'tests/cpp/test_dictionary_block.cpp' -GitHubOutput $outputPath | ConvertFrom-Json)
    Assert-NativeCi ($metadata.native_test_mode -eq 'component' -and -not $metadata.gpu_execution_qualified) 'real entry emits explicit CPU-only component metadata'
    $outputs = @(Get-Content -LiteralPath $outputPath)
    Assert-NativeCi (($outputs -join '|') -eq 'native_build=true|native_tests=true|format_matrix=false|policy_scan=false') 'actual GitHub output values are fixed booleans'
} finally {
    if (Test-Path -LiteralPath $outputPath) { Remove-Item -LiteralPath $outputPath }
    Remove-Item -LiteralPath $fixtureRoot
}
# Bind the production executor to a controlled command boundary; no product workload is launched.
. (Join-Path $PSScriptRoot 'native_ci.ps1')
$script:NativeCiCommands = [Collections.Generic.List[object]]::new()
$script:NativeCiFailure = $false

# Purpose: Observe the production hosted executor's exact command without running native code.
# Inputs: Command is its canonical descriptor; fixture state can fail the single command boundary.
# Outputs: Captures one invocation or throws the original controlled diagnostic.
function Invoke-SuperZipVerificationCommand {
    param($Command)
    $script:NativeCiCommands.Add($Command)
    if ($script:NativeCiFailure) { throw 'Native CI controlled execution failure.' }
}

$component = Get-SuperZipHostedNativePlan -Paths @('tests/cpp/test_dictionary_block.cpp')
$original = $component.testCommand.arguments -join '|'
Invoke-SuperZipHostedNativeTest -Plan $component | Out-Null
Assert-NativeCi ($script:NativeCiCommands.Count -eq 1 -and
    '-CpuOnlyValidation' -in $script:NativeCiCommands[0].arguments -and
    ($component.testCommand.arguments -join '|') -eq $original) 'component execution explicitly labels CPU mode without mutating the plan'
$script:NativeCiCommands.Clear()
Invoke-SuperZipHostedNativeTest -Plan $full | Out-Null
Assert-NativeCi ($script:NativeCiCommands.Count -eq 1 -and '-CpuOnlyValidation' -notin $script:NativeCiCommands[0].arguments) 'broad execution retains the existing driver once'
$script:NativeCiCommands.Clear()
Invoke-SuperZipHostedNativeTest -Plan (Get-SuperZipHostedNativePlan -Paths @('docs/design.md')) | Out-Null
Assert-NativeCi ($script:NativeCiCommands.Count -eq 0) 'no selected native mechanism launches no native command'
$script:NativeCiFailure = $true
$rejected = $false
try { Invoke-SuperZipHostedNativeTest -Plan $component | Out-Null } catch { $rejected = $_.Exception.Message -eq 'Native CI controlled execution failure.' }
Assert-NativeCi ($rejected -and $script:NativeCiCommands.Count -eq 1) 'native command failure remains terminal and preserves its original cause'
Write-Output 'Hosted native event, projection, trigger parity, conditional policy and qualification contracts passed.'
