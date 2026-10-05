$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

# Purpose: Throw a readable assertion failure for verification selector tests.
# Inputs: `Condition` is the assertion result and `Message` describes the expected behavior.
# Outputs: Throws when `Condition` is false.
function Assert-Selector {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "verification selector assertion failed: $Message"
    }
}

# Purpose: Return command ids from a verification plan.
# Inputs: `Plan` is produced by `Get-SuperZipVerificationPlan`.
# Outputs: Returns local required command ids.
function Get-RequiredCommandId {
    param([Parameter(Mandatory = $true)]$Plan)

    return @($Plan.requiredLocalCommands | ForEach-Object { $_.id })
}

# Purpose: Read the shared push/PR path filter from the two owned workflow fixtures.
# Inputs: Repository-relative workflow name and a candidate changed path.
# Outputs: Returns whether its anchored positive globs admit the path; rejects missing/shared-filter drift.
function Test-OwnedWorkflowPathFilter {
    param([string]$Workflow, [string]$Path)

    $root = Split-Path -Parent $PSScriptRoot
    $source = Get-Content -LiteralPath (Join-Path $root ".github/workflows/$Workflow.yml") -Raw
    $filter = [regex]::Match($source, '(?m)^    paths: &(?<anchor>[\w-]+)\r?\n(?<paths>(?:      - [^\r\n]+\r?\n)+)')
    Assert-Selector $filter.Success "owned workflow must declare a bounded anchored filter: $Workflow"
    $alias = "    paths: *$($filter.Groups['anchor'].Value)"
    Assert-Selector ($source.Contains($alias)) "push and PR must share the same filter: $Workflow"
    foreach ($line in ($filter.Groups['paths'].Value -split '\r?\n')) {
        if (-not $line) { continue }
        $glob = $line.Substring(8)
        Assert-Selector ($glob -notmatch '[!\[\]?]') "fixture reader supports only owned positive literal/star globs: $Workflow"
        if ($Path -clike $glob) { return $true }
    }
    return $false
}

# Purpose: Test that a plan contains a required local command id.
# Inputs: `Plan` is a verification plan and `Id` is the expected command id.
# Outputs: Returns true when the command is present.
function Test-RequiredCommand {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Id
    )

    return @($Plan.requiredLocalCommands | Where-Object { $_.id -eq $Id }).Count -gt 0
}

# Purpose: Test that a plan selects a post-push workflow.
# Inputs: `Plan` is a verification plan and `Name` is the expected workflow name.
# Outputs: Returns true when the workflow is selected.
function Test-Workflow {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return @($Plan.postPushWorkflows | Where-Object { $_ -eq $Name }).Count -gt 0
}

# Purpose: Test that a plan selects a long-running post-push workflow for observation.
# Inputs: `Plan` is a verification plan and `Name` is the expected workflow name.
# Outputs: Returns true when the long-running workflow is selected.
function Test-LongRunningWorkflow {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return @($Plan.longRunningPostPushWorkflows | Where-Object { $_ -eq $Name }).Count -gt 0
}

# Purpose: Invoke the workflow waiter in a no-network mode for selector smoke coverage.
# Inputs: `Arguments` are passed to `wait_relevant_workflows.ps1`.
# Outputs: Returns the native process exit code.
function Invoke-WaiterSmoke {
    param([string[]]$Arguments)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "powershell"
    $startInfo.WorkingDirectory = (Split-Path -Parent $PSScriptRoot)
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $allArguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (Join-Path $PSScriptRoot "wait_relevant_workflows.ps1")
    ) + @($Arguments)
    $startInfo.Arguments = (@($allArguments) | ForEach-Object { '"' + ([string]$_ -replace '"', '\"') + '"' }) -join " "

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    return $process.ExitCode
}

$emptyPaths = @(Select-SuperZipUniquePath -Path @($null, "", " "))
foreach ($path in @('README.md', 'src/cli/main.cpp', 'docs/benchmarks/data/new-study.json', 'tools/scanner_preflight.py')) {
    $preflightPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $preflightPlan -Id 'scanner-preflight') "changed publication inputs require actual scanner preflight: $path"
}
$preflightToolPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/scanner_preflight.py')
Assert-Selector (Test-RequiredCommand -Plan $preflightToolPlan -Id 'scanner-preflight-tests') 'scanner admission changes require boundary contracts'
Assert-Selector (-not (Test-RequiredCommand -Plan $preflightToolPlan -Id 'release-build')) 'scanner admission alone must not rebuild the app'
Assert-Selector (-not (Test-SuperZipToolContractCommand -Command ([pscustomobject]@{id='scanner-preflight'}))) 'offline contracts must not pretend to execute provisioned detectors'
foreach ($workflow in @('fuzzing', 'greenbone-openvas-vulnetix')) {
    $source = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) ".github/workflows/$workflow.yml") -Raw
    Assert-Selector ($source -match '(?m)^  workflow_dispatch:\s*$') "final qualification must support manual dispatch when path filters omit a run: $workflow"
}
Assert-Selector ($emptyPaths.Count -eq 0) "clean git output must normalize to an empty path set without binding errors"

foreach ($path in @('tools/fuzz_resources.ps1', 'tools/test_fuzz_resources.ps1',
        'tools/fuzz_memory.py', 'tools/test_fuzz_memory.py')) {
    $resourcePlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector (-not $resourcePlan.scope.fullEscalationRequired) 'fuzz resource boundaries require focused resource contracts'
    $resourceContract = if ($path -match '\.ps1$') { 'fuzz-resource-tests' } else { 'fuzz-memory-tests' }
    Assert-Selector (Test-RequiredCommand -Plan $resourcePlan -Id $resourceContract) 'changed resource mechanism must retain its regressions'
    $expectsIntegration = $path -notmatch '/test_'
    Assert-Selector ((Test-RequiredCommand -Plan $resourcePlan -Id 'short-fuzz-smoke') -eq $expectsIntegration) 'production resource changes require actual sanitizer admission; test-only changes do not'
}

$docsPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/targeted-verification.md")
Assert-Selector $docsPlan.scope.docsOnly "docs-only changes must be classified as docsOnly"
Assert-Selector (-not $docsPlan.scope.fullEscalationRequired) "docs-only changes must not escalate"
Assert-Selector ((Get-RequiredCommandId -Plan $docsPlan).Count -eq 3) "docs-only changes must require hygiene, actual scanner admission and language lint"
Assert-Selector (Test-RequiredCommand -Plan $docsPlan -Id 'scanner-preflight') 'documentation inputs must retain actual detector admission'
Assert-Selector (Test-RequiredCommand -Plan $docsPlan -Id "language-lint") "docs-only changes must run markdown lint through the language linter"
Assert-Selector (-not (Test-RequiredCommand -Plan $docsPlan -Id "lint-routing-tests")) "docs-only changes must not rerun unchanged routing implementation tests"
$lintPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/lint.ps1")
Assert-Selector (Test-RequiredCommand -Plan $lintPlan -Id "lint-routing-tests") "linter implementation changes must test production file routing"
$base64Index = [Array]::IndexOf([object[]]$docsPlan.requiredLocalCommands[0].arguments, "-ChangedPathBase64")
Assert-Selector ($base64Index -ge 0) "hygiene command must use deterministic Base64 JSON path handoff"
$decodedPaths = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($docsPlan.requiredLocalCommands[0].arguments[$base64Index + 1])) | ConvertFrom-Json
Assert-Selector (@($decodedPaths)[0] -eq "docs/targeted-verification.md") "hygiene command must receive the exact planned changed path"
Assert-Selector (Test-Workflow -Plan $docsPlan -Name "lint") "docs-only changes must select the fast lint workflow"
Assert-Selector (-not $docsPlan.workflowWaitPolicy.deferAllowed) "unspecified checkpoint must default to final acceptance"
Assert-Selector ($docsPlan.workflowWaitPolicy.recommendedMode -eq "final") "final docs acceptance must require relevant lint"

$sourcePlan = Get-SuperZipVerificationPlan -ChangedPath @("src/core/checksum.cpp")
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "language-lint") "C++ source changes must run the language linter"
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "release-build") "C++ source changes must build"
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "unit-tests") "C++ source changes must run tests"
Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id "changed-refactor-audit") "C++ source changes must run changed refactor audit"
Assert-Selector (Test-Workflow -Plan $sourcePlan -Name "windows-ci") "C++ source changes must wait for windows-ci"
Assert-Selector ($sourcePlan.manualLocalCommands.Count -eq 0) "ordinary non-performance source changes must not require manual benchmark sweeps"
Assert-Selector (-not $sourcePlan.workflowWaitPolicy.deferAllowed) "final source acceptance must not defer workflow waiting"
Assert-Selector ($sourcePlan.workflowWaitPolicy.recommendedMode -eq "final") "source acceptance must recommend final wait"

$entropyPlan = Get-SuperZipVerificationPlan -ChangedPath @('src/gpu/hip_codec_static_prefix.hip.cpp', 'src/gpu/hip_codec_adaptive_prefix.hip.cpp')
$entropyCommand = @($entropyPlan.requiredLocalCommands | Where-Object id -eq 'unit-tests')
Assert-Selector ($entropyCommand.Count -eq 1 -and 'tools/native_component_tests.ps1' -in $entropyCommand[0].arguments) 'reviewed encoder mechanisms must select the component driver once'
Assert-Selector (-not (Test-SuperZipToolContractCommand -Command $entropyCommand[0])) 'native component execution must not enter the offline tool lane'
foreach ($testPath in @('tests/cpp/test_zip_compat.cpp', 'tests/cpp/test_archive_roundtrip.cpp', 'tests/cpp/test_path_safety.cpp')) {
    $testOnlyPlan = Get-SuperZipVerificationPlan -ChangedPath @($testPath)
    $testCommand = @($testOnlyPlan.requiredLocalCommands | Where-Object id -eq 'unit-tests')[0]
    Assert-Selector ('tools/native_component_tests.ps1' -in $testCommand.arguments) "registered test edits select their own cases: $testPath"
    foreach ($id in @('compatibility-interop-smoke', 'format-matrix-smoke', 'short-fuzz-smoke', 'gui-smoke', 'package-smoke')) {
        Assert-Selector (-not (Test-RequiredCommand -Plan $testOnlyPlan -Id $id)) "test-only edits must not select unrelated product work: $testPath / $id"
    }
}
$nativeToolPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/native_test_selection.ps1', 'tools/native_component_tests.ps1')
Assert-Selector (Test-RequiredCommand -Plan $nativeToolPlan -Id 'native-selection-contracts') 'component driver changes require actual selection/execution contracts'
Assert-Selector (-not (Test-RequiredCommand -Plan $nativeToolPlan -Id 'unit-tests')) 'component driver contracts do not require an unchanged native suite'
$nativeRunnerPlan = Get-SuperZipVerificationPlan -ChangedPath @('tests/cpp/test_main.cpp')
Assert-Selector (Test-RequiredCommand -Plan $nativeRunnerPlan -Id 'native-runner-contracts') 'runner source changes require compiled production registry contracts'
Assert-Selector (-not (Test-RequiredCommand -Plan $nativeRunnerPlan -Id 'unit-tests')) 'runner-only edits must not repeat unrelated codec cases'
$sharedNativePlan = Get-SuperZipVerificationPlan -ChangedPath @('src/gpu/hip_codec_support.hpp', 'tools/rocm_toolchain.ps1')
Assert-Selector (-not (Test-RequiredCommand -Plan $sharedNativePlan -Id 'rocm-toolchain-tests')) 'full native driver already executes compiler contracts'
$componentToolPlan = Get-SuperZipVerificationPlan -ChangedPath @('src/gpu/hip_codec_static_prefix.hip.cpp', 'tools/rocm_toolchain.ps1')
Assert-Selector (Test-RequiredCommand -Plan $componentToolPlan -Id 'rocm-toolchain-tests') 'component driver must retain separately changed compiler contracts'

$gpuPlan = Get-SuperZipVerificationPlan -ChangedPath @("src/gpu/dictionary_candidate.cpp")
Assert-Selector $gpuPlan.scope.touchesPerformance "GPU codec changes must retain performance verification"
Assert-Selector (($gpuPlan.requiredLocalCommands | Where-Object id -eq 'unit-tests').command -match 'native_component_tests\.ps1') 'reviewed dictionary mechanism must execute its canonical component cohort'
Assert-Selector (-not (Test-Workflow -Plan $gpuPlan -Name "benchmark-graph")) "GPU source alone must not wait for a path-filtered graph workflow"

foreach ($case in @(
    @('.github/workflows/fuzzing.yml', $true), @('.clusterfuzzlite/build.sh', $true),
    @('tools/test_fuzz_build.py', $true), @('fuzz/archive_index_fuzzer.cpp', $true),
    @('src/core/checksum.cpp', $true), @('src/iso/iso_adapter.cpp', $true),
    @('third_party/miniz/miniz.c', $true), @('src/gpu/hip_codec.hip.cpp', $false),
    @('src/app/main_window.cpp', $false), @('src/zip/zip_adapter.cpp', $false),
    @('tools/agent_context.py', $false), @('docs/targeted-verification.md', $false)
)) {
    $selected = Test-LongRunningWorkflow -Plan (Get-SuperZipVerificationPlan -ChangedPath @($case[0])) -Name 'fuzzing'
    Assert-Selector ($selected -eq $case[1]) "hosted fuzz input ownership: $($case[0])"
    Assert-Selector ((Test-OwnedWorkflowPathFilter -Workflow fuzzing -Path $case[0]) -eq $selected) "fuzz planner/trigger parity: $($case[0])"
}
$root = Split-Path -Parent $PSScriptRoot
$fuzzBuild = Get-Content -LiteralPath (Join-Path $root '.clusterfuzzlite/build.sh') -Raw
foreach ($dependency in [regex]::Matches($fuzzBuild, '(?:src|fuzz|third_party)/[\w./-]+\.(?:cpp|c|hpp|h)\b')) {
    Assert-Selector (Test-OwnedWorkflowPathFilter -Workflow fuzzing -Path $dependency.Value) "actual declared fuzzer input must trigger hosted fuzzing: $($dependency.Value)"
}
foreach ($case in @(
    @('.github/openvas/resolve_config.cjs', $true), @('.github/workflows/greenbone-openvas-vulnetix.yml', $true),
    @('.github/workflows/greenbone-openvas-live.yml', $true), @('.github/requirements/requirements-gvm-tools-linux.txt', $true),
    @('tools/agent_context.py', $false), @('src/core/checksum.cpp', $false), @('docs/targeted-verification.md', $false)
)) {
    $selected = Test-Workflow -Plan (Get-SuperZipVerificationPlan -ChangedPath @($case[0])) -Name 'greenbone-openvas-vulnetix'
    Assert-Selector ($selected -eq $case[1]) "offline Greenbone input ownership: $($case[0])"
    Assert-Selector ((Test-OwnedWorkflowPathFilter -Workflow greenbone-openvas-vulnetix -Path $case[0]) -eq $selected) "Greenbone planner/trigger parity: $($case[0])"
}

$archivePlan = Get-SuperZipVerificationPlan -ChangedPath @("src/zip/zip_adapter.cpp")
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "security-scan") "archive parser changes must run security scan"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "compatibility-interop-smoke") "archive parser changes must run external compatibility interop smoke"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "format-matrix-smoke") "archive parser changes must run the registry-wide format matrix smoke"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "short-fuzz-smoke") "archive parser changes must run short fuzz smoke"
Assert-Selector (-not (Test-Workflow -Plan $archivePlan -Name "fuzzing")) "archive parser changes must not block normal waits on fuzzing"
Assert-Selector (-not (Test-LongRunningWorkflow -Plan $archivePlan -Name "fuzzing")) "ZIP is not compiled by the hosted fuzz build and must not start unrelated sanitizer jobs"

$guiPlan = Get-SuperZipVerificationPlan -ChangedPath @("src/app/main_window.cpp")
Assert-Selector (Test-RequiredCommand -Plan $guiPlan -Id "gui-smoke") "GUI changes must run GUI smoke"
Assert-Selector (Test-Workflow -Plan $guiPlan -Name "windows-ci") "GUI changes must wait for windows-ci"

$guiHelperPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/SuperZip.GuiSmoke.Ui.psm1")
Assert-Selector $guiHelperPlan.scope.touchesGui "GUI smoke helpers must be classified as GUI changes"
Assert-Selector (Test-RequiredCommand -Plan $guiHelperPlan -Id "gui-smoke") "GUI smoke helper changes must exercise the GUI"
Assert-Selector (Test-Workflow -Plan $guiHelperPlan -Name "windows-ci") "GUI smoke helper changes must select Windows CI"

foreach ($path in @("src/core/archive_name_encoding.cpp", "src/core/archive_name_encoding.hpp")) {
    $encodingPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector $encodingPlan.scope.touchesArchiveParser "archive name decoding must be classified as a parser boundary: $path"
    Assert-Selector (Test-RequiredCommand -Plan $encodingPlan -Id "format-matrix-smoke") "name decoding changes must run the format matrix: $path"
    Assert-Selector (Test-LongRunningWorkflow -Plan $encodingPlan -Name "fuzzing") "name decoding changes must observe fuzzing: $path"
}

$componentCases = @(
    @('.clusterfuzzlite/build.sh', 'short-fuzz-smoke'),
    @('.clusterfuzzlite/local_smoke.sh', 'fuzz-memory-tests'),
    @('.clusterfuzzlite/Dockerfile', 'fuzz-resource-tests'),
    @('.clusterfuzzlite/project.yaml', 'short-fuzz-smoke'),
    @('tools/test_verification_selector.ps1', 'verification-selector-self-test'),
    @('tools/build_parallelism.ps1', 'build-parallelism-test'),
    @('tools/test_build_parallelism.ps1', 'build-parallelism-test'),
    @('tools/analyze_hip_trace.py', 'hip-api-trace-tests'),
    @('tools/test_hip_trace.py', 'hip-api-trace-tests'),
    @('tools/refactor_audit.ps1', 'refactor-audit-tests'),
    @('tools/test_refactor_audit.ps1', 'refactor-audit-tests')
)
foreach ($case in $componentCases) {
    $componentPlan = Get-SuperZipVerificationPlan -ChangedPath @($case[0])
    Assert-Selector (-not $componentPlan.scope.fullEscalationRequired) "tool changes must select component contracts, not all product checks: $($case[0])"
    Assert-Selector (Test-RequiredCommand -Plan $componentPlan -Id $case[1]) "required component contract missing: $($case[0])"
    Assert-Selector (-not (Test-RequiredCommand -Plan $componentPlan -Id 'gui-smoke')) "tool changes must not launch the GUI: $($case[0])"
    Assert-Selector (-not (Test-RequiredCommand -Plan $componentPlan -Id 'package-smoke')) "tool changes must not package the product: $($case[0])"
}

foreach ($path in @('tools/analyze_hip_trace.py', 'tools/test_hip_trace.py')) {
    $tracePlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    foreach ($id in @('release-build', 'unit-tests', 'benchmark-tooling-tests', 'ram-benchmark-sweep')) {
        Assert-Selector (-not (Test-RequiredCommand -Plan $tracePlan -Id $id)) "offline HIP trace tools must not rerun native workloads or graph checks: $path, $id"
    }
    Assert-Selector (Test-Workflow -Plan $tracePlan -Name 'component-contracts') "trace contracts must run on the hosted component path: $path"
}

$cmakeManifestPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/cmake-toolchain.sha256')
Assert-Selector $cmakeManifestPlan.scope.touchesVerification 'CMake provenance must be classified as verification input'
Assert-Selector (Test-RequiredCommand -Plan $cmakeManifestPlan -Id 'release-build') 'Changed CMake provenance requires a native build'
Assert-Selector (Test-RequiredCommand -Plan $cmakeManifestPlan -Id 'cmake-toolchain-tests') 'Changed CMake provenance requires its direct contracts'
foreach ($workflow in @('windows-ci', 'rocm-qualification', 'zstd-sanitizers')) {
    Assert-Selector (Test-Workflow -Plan $cmakeManifestPlan -Name $workflow) "CMake provenance must select $workflow"
    Assert-Selector (Test-OwnedWorkflowPathFilter -Workflow $workflow -Path 'tools/cmake-toolchain.sha256') "CMake provenance must trigger $workflow"
}

$workflowPlan = Get-SuperZipVerificationPlan -ChangedPath @(".github/workflows/security-code-scanning.yml")
foreach ($path in @('tools/native_build_provenance.py', 'tools/native_build_receipt.py',
        'tools/test_native_build_provenance.py', 'tools/test_native_build_receipt.py')) {
    $receiptPlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector $receiptPlan.scope.touchesVerification "Receipt producer/consumer changes require verification coverage: $path"
    Assert-Selector (Test-RequiredCommand -Plan $receiptPlan -Id 'native-build-receipt-tests') "Receipt contracts must be executable: $path"
    $expectsBuild = $path -match '^tools/native_build_'
    Assert-Selector ((Test-RequiredCommand -Plan $receiptPlan -Id 'release-build') -eq $expectsBuild) "only production receipt inputs require rebuilding artifacts: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $receiptPlan -Id 'unit-tests')) "receipt-only changes must not rerun unrelated codec tests: $path"
}
foreach ($path in @('tools/rocm_toolchain.ps1', 'tools/bootstrap_rocm_sdk.py', 'tools/test_bootstrap_rocm_sdk.py',
        'tools/test_rocm_toolchain.ps1', 'tools/rocm-sdk-lock.json', 'tools/compile_hip_object.ps1',
        'tools/process_environment.ps1', 'tools/test_process_environment.ps1')) {
    $rocmPlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector $rocmPlan.scope.touchesVerification "ROCm build/provisioning inputs require compiler and verifier coverage: $path"
    $expectedContract = switch -Regex ($path) {
        'bootstrap_rocm_sdk|rocm-sdk-lock' { 'rocm-bootstrap-tests'; break }
        'process_environment' { 'process-environment-tests'; break }
        default { 'rocm-toolchain-tests' }
    }
    Assert-Selector (Test-RequiredCommand -Plan $rocmPlan -Id $expectedContract) "ROCm changes require their component contracts: $path"
    $expectsBuild = $path -notmatch '/test_'
    Assert-Selector ((Test-RequiredCommand -Plan $rocmPlan -Id 'release-build') -eq $expectsBuild) "only production build inputs require rebuilding artifacts: $path"
}
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "security-scan") "workflow changes must run security scan"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "lint") "workflow changes must wait for lint"
Assert-Selector (-not (Test-Workflow -Plan $workflowPlan -Name "benchmark-graph")) "scanner workflow changes must not dispatch unrelated graph validation"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "security") "workflow changes must wait for security"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "scorecard") "workflow changes must wait for scorecard"
Assert-Selector $workflowPlan.postPushAuditRequired "workflow changes must require post-push audit"
$benchmarkPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/benchmarks/data/effort-native-L5.json")
Assert-Selector (Test-Workflow -Plan $benchmarkPlan -Name "benchmark-graph") "benchmark records must wait for graph regeneration"
Assert-Selector (Test-RequiredCommand -Plan $benchmarkPlan -Id "benchmark-tooling-tests") "benchmark data must retain graph contracts"
$svgPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/render_svg_graph.py', 'tools/test_svg_graph.py', 'resources/benchmarks/chart-template.svg')
Assert-Selector (-not $svgPlan.scope.requiresClassificationReview) 'shared SVG document changes must have an explicit graph classification'
$svgTests = @($svgPlan.requiredLocalCommands | Where-Object { $_.id -eq 'benchmark-tooling-tests' })
Assert-Selector ($svgTests.Count -eq 1 -and $svgTests[0].arguments -contains 'tools.test_svg_graph') 'shared SVG document changes must run their passive-markup contracts'
Assert-Selector (Test-Workflow -Plan $svgPlan -Name 'benchmark-graph') 'shared SVG document changes must select hosted byte-exact graph checks'
Assert-Selector (-not (Test-RequiredCommand -Plan $svgPlan -Id 'release-build')) 'shared SVG documentation tooling must not trigger a product rebuild'
foreach ($path in @('docs/benchmarks/corpora.md', 'docs/benchmarks/household-power-diagnostic-2026-10-03.md')) {
    $benchmarkNarrativePlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkNarrativePlan -Id 'benchmark-tooling-tests')) "benchmark narrative alone must not rerun unchanged graph contracts: $path"
    Assert-Selector (-not (Test-Workflow -Plan $benchmarkNarrativePlan -Name 'benchmark-graph')) "benchmark narrative alone must not wait for a path-filtered graph workflow: $path"
    Assert-Selector (Test-RequiredCommand -Plan $benchmarkNarrativePlan -Id 'changed-hygiene') "benchmark narrative retains source hygiene: $path"
    Assert-Selector (Test-RequiredCommand -Plan $benchmarkNarrativePlan -Id 'language-lint') "benchmark narrative retains documentation lint: $path"
}
$benchmarkCorpusPinPlan = Get-SuperZipVerificationPlan -ChangedPath @('docs/benchmarks/corpora/household-power-source.json')
Assert-Selector (-not (Test-Workflow -Plan $benchmarkCorpusPinPlan -Name 'benchmark-graph')) 'corpus configuration is not measurement graph input'
Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkCorpusPinPlan -Id 'benchmark-tooling-tests')) 'corpus configuration does not select unrelated graph contracts'
Assert-Selector (Test-RequiredCommand -Plan $benchmarkCorpusPinPlan -Id 'benchmark-corpus-acquisition-test') 'nested source pins retain acquisition contracts'
Assert-Selector (Test-Workflow -Plan $benchmarkCorpusPinPlan -Name 'security') 'download configuration retains hosted scanner coverage'
foreach ($path in @('.github/scanner-metadata-reviews.csv', 'tools/scanner_metadata_review.py', 'tools/test_scanner_metadata_review.py')) {
    $metadataReviewPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $metadataReviewPlan -Id 'scanner-metadata-review-tests') "metadata review inputs retain exact snapshot contracts: $path"
    Assert-Selector (Test-Workflow -Plan $metadataReviewPlan -Name 'security') "metadata review inputs retain hosted scans: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $metadataReviewPlan -Id 'release-build')) "metadata review alone must not build the product: $path"
}
foreach ($path in @("tools/benchmark_cache.py", "tools/test_benchmark_cache.py", "tools/benchmark_comparators.py", "tools/test_benchmark_comparators.py", "tools/benchmark_permissions.json", "docs/benchmark-permissions.md", "docs/benchmark-research.md", "docs/comparative-benchmark-methodology.md")) {
    $benchmarkToolPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "benchmark-tooling-tests") "benchmark tools and policies require offline validation: $path"
    Assert-Selector (Test-Workflow -Plan $benchmarkToolPlan -Name "benchmark-graph") "benchmark tooling changes must select graph validation: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "release-build")) "benchmark tooling alone must not rebuild the application: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "gui-smoke")) "benchmark tooling alone must not launch the GUI: $path"
}
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "secret-report-tests") "workflow changes must test secret artifact redaction"
foreach ($path in @('.github/gitleaks.toml', 'tools/test_gitleaks_policy.py')) {
    $secretPolicyPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $secretPolicyPlan -Id 'gitleaks-policy-tests') "checksum policy must test its narrow exceptions: $path"
    Assert-Selector (Test-Workflow -Plan $secretPolicyPlan -Name 'security') "checksum policy must exercise the hosted secret scanners: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $secretPolicyPlan -Id 'release-build')) "secret scanner policy alone must not rebuild the product: $path"
}
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "scanner-coverage-tests") "workflow changes must test complete scanner coverage evidence"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "devskim-provenance-tests") "workflow changes must test exact scanner package provenance"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "devskim-report-tests") "workflow changes must test lossless scanner report publication"
Assert-Selector (-not (Test-RequiredCommand -Plan $workflowPlan -Id "greenbone-config-tests")) "scanner workflow changes must not test an unrelated broker"

foreach ($path in @("tools/semgrep_coverage.py", "tools/test_semgrep_coverage.py")) {
    $coveragePlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $coveragePlan -Id "scanner-coverage-tests") "coverage changes must run their offline regressions: $path"
    Assert-Selector (Test-Workflow -Plan $coveragePlan -Name "security") "coverage changes must exercise the real scanner report: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $coveragePlan -Id "release-build")) "coverage metadata changes alone must not rebuild the product: $path"
}

foreach ($path in @("tools/devskim_provenance.py", "tools/test_devskim_provenance.py", ".github/requirements/devskim-packaging.json")) {
    $provenancePlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $provenancePlan -Id "devskim-provenance-tests") "scanner provenance changes must run their offline regressions: $path"
    Assert-Selector (Test-Workflow -Plan $provenancePlan -Name "security") "scanner provenance changes must exercise the real installation: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $provenancePlan -Id "release-build")) "scanner metadata changes alone must not rebuild the product: $path"
}
foreach ($path in @("tools/devskim_report.py", "tools/test_devskim_report.py")) {
    $reportPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $reportPlan -Id "devskim-report-tests") "report changes must run their offline regressions: $path"
    Assert-Selector (Test-Workflow -Plan $reportPlan -Name "security") "report changes must exercise hosted SARIF validation: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $reportPlan -Id "release-build")) "scanner report changes alone must not rebuild the product: $path"
}

foreach ($path in @("tools/test_semgrep_installation.py", "tools/test_semgrep_runtime.py", ".github/requirements/requirements-semgrep-linux.in", ".github/requirements/requirements-semgrep-linux.txt", ".github/workflows/security-code-scanning.yml")) {
    $scannerPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $scannerPlan -Id "scanner-installation-tests") "scanner changes require official pin, hash and production-install regressions: $path"
    Assert-Selector (Test-Workflow -Plan $scannerPlan -Name "security") "scanner changes require Linux runtime checks in the hosted security workflow: $path"
}

foreach ($path in @("tools/redact_trufflehog.py", "tools/test_redact_trufflehog.py", "tools/scan_trufflehog.sh",
        ".github/scanner-secret-reviews.json", ".github/scanner-secret-reviews-crawl4ai.json",
        "third_party/upstream/nltk/source.zip")) {
    $redactionPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (-not $redactionPlan.scope.fullEscalationRequired) "secret report changes require their contracts, not product-wide tests: $path"
    Assert-Selector (Test-RequiredCommand -Plan $redactionPlan -Id "secret-report-tests") "redaction changes must execute their regressions: $path"
    Assert-Selector (Test-Workflow -Plan $redactionPlan -Name "security") "redaction changes must require hosted scanner validation: $path"
}

$packagingPlan = Get-SuperZipVerificationPlan -ChangedPath @("CMakeLists.txt")
foreach ($guardPath in @('tools/zstd_rewrite_policy.ps1', 'tools/test_zstd_rewrite_policy.ps1',
        'cmake/ZstdRawBlockWriter.c', 'cmake/ZstdLegacyStreamV05.c', 'tests/cpp/test_zstd_bounds.cpp',
        'tools/security_scan.ps1', 'tools/verify_change_hygiene.ps1')) {
    $guardPlan = Get-SuperZipVerificationPlan -ChangedPath @($guardPath)
    Assert-Selector (Test-RequiredCommand -Plan $guardPlan -Id 'zstd-rewrite-policy-contracts') "Zstandard recurrence guards must execute for their inputs and consumers: $guardPath"
    Assert-Selector (Test-SuperZipToolContractCommand -Command @($guardPlan.requiredLocalCommands | Where-Object { $_.id -eq 'zstd-rewrite-policy-contracts' })[0]) "Zstandard guard must project into hosted tool contracts: $guardPath"
}
foreach ($guardPath in @('tools/zstd_rewrite_policy.ps1', 'tools/test_zstd_rewrite_policy.ps1')) {
    $guardPlan = Get-SuperZipVerificationPlan -ChangedPath @($guardPath)
    Assert-Selector (-not (Test-RequiredCommand -Plan $guardPlan -Id 'release-build')) "policy-only edits must not require unrelated native rebuilds: $guardPath"
    Assert-Selector (Test-Workflow -Plan $guardPlan -Name 'security') "Zstandard guard retains hosted security validation: $guardPath"
    Assert-Selector (Test-OwnedWorkflowPathFilter -Workflow 'component-contracts' -Path $guardPath) "Zstandard guard inputs must trigger hosted contracts: $guardPath"
}
foreach ($sanitizerPath in @('tools/test_zstd_sanitizers.ps1', '.github/workflows/zstd-sanitizers.yml',
        'tools/cmake_toolchain.ps1', 'tools/cmake-toolchain.sha256', 'tools/build_parallelism.ps1', 'tools/local_resources.ps1',
        'tools/process_environment.ps1', 'tools/native_build_provenance.py', 'tests/cpp/test_util.hpp',
        'tests/cpp/test_main.cpp',
        'cmake/ZstdFutureBoundary.c', 'tests/zstd/future/header.c', 'tests/cpp/test_zstd_future.cpp',
        'third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip', 'tests/cpp/zstd_legacy_fixture.hpp', 'cmake/ZstdLibrary.cmake',
        'cmake/ZstdTests.cmake', 'cmake/ZstdRawBlockWriter.c', 'tests/zstd/sanitizers/CMakeLists.txt',
        'tests/zstd/sanitizers/asan_control.cpp', 'tests/cmake/test_zstd_asan_control.cmake',
        'tests/cpp/test_zstd_bounds.cpp', 'CMakeLists.txt')) {
    $sanitizerPlan = Get-SuperZipVerificationPlan -ChangedPath @($sanitizerPath)
    Assert-Selector (Test-RequiredCommand -Plan $sanitizerPlan -Id 'zstd-sanitizer-tests') "Zstandard memory-safety surfaces require qualified sanitizer validation: $sanitizerPath"
    Assert-Selector (-not (Test-SuperZipToolContractCommand -Command @($sanitizerPlan.requiredLocalCommands | Where-Object { $_.id -eq 'zstd-sanitizer-tests' })[0])) "native sanitizer qualification must not run in offline tool contracts: $sanitizerPath"
    Assert-Selector (Test-Workflow -Plan $sanitizerPlan -Name 'zstd-sanitizers') "sanitizer inputs must select their dedicated hosted workflow: $sanitizerPath"
    Assert-Selector (Test-OwnedWorkflowPathFilter -Workflow 'zstd-sanitizers' -Path $sanitizerPath) "sanitizer inputs must trigger hosted validation: $sanitizerPath"
}

foreach ($unrelatedPath in @('tools/scanner_preflight.py', '.github/scanner-source-reviews.csv', 'docs/security-code-scanning.md')) {
    $unrelatedPlan = Get-SuperZipVerificationPlan -ChangedPath @($unrelatedPath)
    Assert-Selector (-not (Test-RequiredCommand -Plan $unrelatedPlan -Id 'zstd-sanitizer-tests')) "unrelated tooling/docs must not rerun native ASan: $unrelatedPath"
    Assert-Selector (-not (Test-OwnedWorkflowPathFilter -Workflow 'zstd-sanitizers' -Path $unrelatedPath)) "unrelated tooling/docs must not trigger the sanitizer workflow: $unrelatedPath"
}
$sanitizerToolPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/test_zstd_sanitizers.ps1')
Assert-Selector (-not (Test-RequiredCommand -Plan $sanitizerToolPlan -Id 'release-build')) 'isolated sanitizer tooling must not select an unrelated product rebuild'
foreach ($releasePath in @('.github/workflows/release.yml', 'tools/release_workflow_policy.ps1',
        'tools/test_release_workflow_policy.ps1')) {
    $releasePlan = Get-SuperZipVerificationPlan -ChangedPath @($releasePath)
    Assert-Selector (Test-RequiredCommand -Plan $releasePlan -Id 'release-workflow-contracts') "release policy must run its actual safeguards: $releasePath"
    Assert-Selector (Test-RequiredCommand -Plan $releasePlan -Id 'security-scan') "release policy must retain repository security checks: $releasePath"
    foreach ($id in @('release-build', 'unit-tests', 'msi-identity-smoke', 'package-smoke')) {
        Assert-Selector (-not (Test-RequiredCommand -Plan $releasePlan -Id $id)) "release orchestration alone must not execute unchanged product work: $releasePath/$id"
    }
    Assert-Selector (-not (Test-Workflow -Plan $releasePlan -Name 'windows-ci')) "release orchestration alone must not require a native CI run: $releasePath"
    Assert-Selector (Test-Workflow -Plan $releasePlan -Name 'security') "release policy retains hosted security audits: $releasePath"
    if ($releasePath -eq '.github/workflows/release.yml') {
        Assert-Selector (Test-Workflow -Plan $releasePlan -Name 'scorecard') 'release workflow changes retain hosted Scorecard'
    }
}
foreach ($producerPath in @('CMakeLists.txt', 'tools/package.ps1')) {
    $producerPlan = Get-SuperZipVerificationPlan -ChangedPath @($producerPath)
    foreach ($id in @('release-build', 'msi-identity-smoke', 'package-smoke')) {
        Assert-Selector (Test-RequiredCommand -Plan $producerPlan -Id $id) "actual release producers retain artifact qualification: $producerPath/$id"
    }
}
foreach ($pchPath in @('cmake/SuperZipPrecompiledHeaders.cmake', 'tests/cmake/precompiled_headers/contract.c',
        'tools/test_precompiled_headers.ps1')) {
    $pchPlan = Get-SuperZipVerificationPlan -ChangedPath @($pchPath)
    Assert-Selector (Test-RequiredCommand -Plan $pchPlan -Id 'precompiled-header-tests') "PCH strategy changes must qualify actual mixed-language consumers: $pchPath"
    Assert-Selector (Test-Workflow -Plan $pchPlan -Name 'component-contracts') "PCH contracts must run in hosted CI: $pchPath"
}
foreach ($releasePath in @('.github/workflows/release.yml')) {
    Assert-Selector (Test-OwnedWorkflowPathFilter -Workflow 'component-contracts' -Path $releasePath) "release policy inputs must trigger hosted component contracts: $releasePath"
}
foreach ($inputPath in @('LICENSE', 'resources/licenses/license-notices.json')) {
    $inputPlan = Get-SuperZipVerificationPlan -ChangedPath @($inputPath)
    Assert-Selector $inputPlan.scope.touchesNativeBuildInputs "compiled license inputs must retain build identity: $inputPath"
    Assert-Selector (Test-RequiredCommand -Plan $inputPlan -Id 'release-build') "compiled license inputs must rebuild their actual outputs: $inputPath"
    Assert-Selector (Test-Workflow -Plan $inputPlan -Name 'windows-ci') "compiled license inputs must retain hosted build coverage: $inputPath"
}


Assert-Selector (Test-RequiredCommand -Plan $packagingPlan -Id "msi-identity-smoke") "packaging changes must run MSI identity smoke"
Assert-Selector (Test-RequiredCommand -Plan $packagingPlan -Id "package-smoke") "packaging changes must run package smoke"
Assert-Selector (Test-Workflow -Plan $packagingPlan -Name "windows-ci") "packaging changes must wait for windows-ci"

$mcpPlan = Get-SuperZipVerificationPlan -ChangedPath @("mcp/superzip_mcp.py")
foreach ($agentToolPath in @("tools/agent_context.py", "tools/test_agent_context.py", "tools/cocoindex_agent_search.py", "tools/test_cocoindex_agent_search.py")) {
    $agentToolPlan = Get-SuperZipVerificationPlan -ChangedPath @($agentToolPath)
    Assert-Selector (-not $agentToolPlan.scope.fullEscalationRequired) "agent context and routing changes require their own contracts"
    Assert-Selector (Test-RequiredCommand -Plan $agentToolPlan -Id "agent-context-contracts") "agent tool changes must exercise real context and search receipt contracts"
    Assert-Selector (-not (Test-RequiredCommand -Plan $agentToolPlan -Id "verification-selector-self-test")) "unchanged planner must not be retested for an agent tool implementation change"
}
Assert-Selector (-not $mcpPlan.scope.fullEscalationRequired) "MCP changes must target bounded child contracts"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "mcp-python-compile") "MCP changes must compile Python"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "mcp-bounded-child-tests") "MCP changes must test bounded child execution"
Assert-Selector (-not (Test-RequiredCommand -Plan $mcpPlan -Id "verification-selector-self-test")) "MCP implementation changes must not retest an unchanged planner"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id 'crawl4ai-contracts') 'process ownership changes must verify the crawler consumer'
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id 'neutron-public-corpus-test') 'process ownership changes must verify the Neutron controller consumer'
foreach ($corpusPath in @('tools/neutron_corpus_benchmark.py', 'tools/neutron_corpus_ipc.py', 'tools/test_neutron_corpus_benchmark.py')) {
    $corpusPlan = Get-SuperZipVerificationPlan -ChangedPath @($corpusPath)
    Assert-Selector (Test-RequiredCommand -Plan $corpusPlan -Id 'neutron-public-corpus-test') 'RAM corpus exchange changes require their actual offline consumer'
    Assert-Selector (-not (Test-RequiredCommand -Plan $corpusPlan -Id 'release-build')) 'offline corpus contracts must not launch a native build'
}
foreach ($crawlerPath in @('tools/crawl4ai_tool.py', 'tools/crawl4ai_research.py', 'tools/test_crawl4ai_tool.py',
        'tools/crawl4ai_source_build.py', 'tools/test_crawl4ai_source_build.py',
        'tools/crawl4ai_download_opener.py', 'tools/test_crawl4ai_downloads.py', 'third_party/upstream/crawl4ai/0.9.4/build.json',
        'tools/nltk_security_build.py', 'tools/test_nltk_security_build.py', 'tools/test_nltk_model_security.py',
        'third_party/upstream/nltk/README.md', 'tools/requirements/nltk-build.txt',
        'tools/crawl4ai_sites.json', 'tools/requirements/crawl4ai.txt', 'docs/crawl4ai.md')) {
    $crawlerPlan = Get-SuperZipVerificationPlan -ChangedPath @($crawlerPath)
    Assert-Selector (Test-RequiredCommand -Plan $crawlerPlan -Id 'crawl4ai-contracts') "crawler inputs require offline contracts: $crawlerPath"
    Assert-Selector (-not $crawlerPlan.scope.fullEscalationRequired) "crawler integration does not imply native product changes: $crawlerPath"
    foreach ($id in @('release-build', 'native-tests', 'gui-smoke', 'verification-selector-self-test')) {
        Assert-Selector (-not (Test-RequiredCommand -Plan $crawlerPlan -Id $id)) "crawler-only checks must not select $id"
    }
}
foreach ($crawlerSourcePath in @('tools/crawl4ai_source_build.py', 'tools/test_crawl4ai_source_build.py',
        'tools/crawl4ai_download_opener.py', 'tools/test_crawl4ai_downloads.py', 'third_party/upstream/crawl4ai/0.9.4/build.json')) {
    $sourcePlan = Get-SuperZipVerificationPlan -ChangedPath @($crawlerSourcePath)
    Assert-Selector (Test-RequiredCommand -Plan $sourcePlan -Id 'crawl4ai-source-contracts') 'crawler source changes require the actual source/platform contract'
    Assert-Selector (-not (Test-Workflow -Plan $sourcePlan -Name 'crawl4ai-security')) 'qualified research setup must not select a retired continuous workflow'
    Assert-Selector (-not (Test-RequiredCommand -Plan $sourcePlan -Id 'release-build')) 'crawler source changes do not build the archive application'
}
foreach ($nltkPath in @('tools/nltk_security_build.py', 'tools/test_nltk_security_build.py', 'tools/test_nltk_model_security.py',
        'tools/requirements/nltk-build.txt', 'tools/requirements/crawl4ai.txt', 'third_party/upstream/nltk/README.md')) {
    $nltkPlan = Get-SuperZipVerificationPlan -ChangedPath @($nltkPath)
    Assert-Selector (Test-RequiredCommand -Plan $nltkPlan -Id 'nltk-source-contracts') "NLTK source admission requires offline contracts: $nltkPath"
    Assert-Selector (-not (Test-Workflow -Plan $nltkPlan -Name 'crawl4ai-security')) "NLTK installation admission must not select a retired continuous workflow: $nltkPath"
    foreach ($productId in @('release-build', 'unit-tests', 'gui-smoke', 'package-smoke')) {
        Assert-Selector (-not (Test-RequiredCommand -Plan $nltkPlan -Id $productId)) "NLTK tooling changes must not invoke unrelated product work: $productId"
    }
}
foreach ($licensePath in @('tools/license_inventory.py', 'tools/test_license_inventory.py', 'docs/licenses/development-notices.json')) {
    $licensePlan = Get-SuperZipVerificationPlan -ChangedPath @($licensePath)
    Assert-Selector (Test-RequiredCommand -Plan $licensePlan -Id 'license-inventory-contracts') "license inventory inputs require their contracts: $licensePath"
    Assert-Selector (-not (Test-RequiredCommand -Plan $licensePlan -Id 'release-build')) 'development notice checks must not build the product'
}

$focusedVerifierPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/superzip_verification.psm1')
Assert-Selector (-not $focusedVerifierPlan.scope.fullEscalationRequired) 'planner changes must execute routing contracts without global escalation'
foreach ($id in @('release-build', 'unit-tests', 'gui-smoke', 'package-smoke', 'short-fuzz-smoke')) {
    Assert-Selector (-not (Test-RequiredCommand -Plan $focusedVerifierPlan -Id $id)) "planner-only changes must not select unrelated product command: $id"
}
# Explicit broad coverage remains available; this fixture is not the default tooling plan.
$verifierPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/superzip_verification.psm1") -SuspectGlobalBug
foreach ($runnerPath in @('tools/verify_changes.ps1', 'tools/test_verification_runner.ps1')) {
    $runnerPlan = Get-SuperZipVerificationPlan -ChangedPath @($runnerPath) -Checkpoint intermediate
    Assert-Selector (Test-RequiredCommand -Plan $runnerPlan -Id 'verification-runner-tests') "runner changes must execute actual failure-propagation contracts: $runnerPath"
}
Assert-Selector $verifierPlan.scope.fullEscalationRequired "verification tool changes must escalate"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "verification-selector-self-test") "verification tool changes must self-test selector"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "msi-identity-smoke") "full escalation must include MSI identity smoke"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "package-smoke") "full escalation must include package smoke"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "compatibility-interop-smoke") "full escalation must include external compatibility interop smoke"
Assert-Selector (Test-RequiredCommand -Plan $verifierPlan -Id "format-matrix-smoke") "full escalation must include the registry-wide format matrix smoke"
Assert-Selector (Test-Workflow -Plan $verifierPlan -Name "greenbone-openvas-vulnetix") "full escalation must include Greenbone/Vulnetix workflow"
Assert-Selector (Test-LongRunningWorkflow -Plan $verifierPlan -Name "fuzzing") "full escalation must observe fuzzing without making it a normal blocking wait"
Assert-Selector $verifierPlan.workflowWaitPolicy.immediateRequired "verification changes must require immediate final workflow waiting"
Assert-Selector (-not $verifierPlan.workflowWaitPolicy.deferAllowed) "verification changes must not allow deferred workflow waiting by default"

foreach ($path in @('docs/targeted-verification.md', 'src/core/checksum.cpp', '.github/workflows/security-code-scanning.yml',
        'mcp/superzip_mcp.py', '.agents/skills/superzip-build-test/SKILL.md', 'tools/superzip_verification.psm1')) {
    $finalPlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint final
    $intermediatePlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector $intermediatePlan.workflowWaitPolicy.deferAllowed "intermediate checkpoints must permit nonblocking observation: $path"
    Assert-Selector (-not $intermediatePlan.workflowWaitPolicy.immediateRequired) "intermediate checkpoints must not force final waiting: $path"
    Assert-Selector $intermediatePlan.workflowWaitPolicy.finalRequired "intermediate checkpoints must retain final acceptance: $path"
    Assert-Selector ($intermediatePlan.workflowWaitPolicy.recommendedMode -eq 'opportunistic') "intermediate checkpoints must sample once: $path"
    foreach ($field in @('scope', 'requiredLocalCommands', 'manualLocalCommands', 'allPostPushWorkflows', 'postPushAuditRequired')) {
        $finalValue = ConvertTo-Json -InputObject $finalPlan.$field -Depth 8 -Compress
        $intermediateValue = ConvertTo-Json -InputObject $intermediatePlan.$field -Depth 8 -Compress
        Assert-Selector ($finalValue -eq $intermediateValue) "checkpoint timing must not change $field coverage: $path"
    }
}
$fullIntermediate = Get-SuperZipVerificationPlan -ChangedPath @('README.md') -SuspectGlobalBug -Checkpoint intermediate
Assert-Selector ($fullIntermediate.scope.fullEscalationRequired -and $fullIntermediate.workflowWaitPolicy.deferAllowed) `
    'full local escalation must not imply blocking intermediate remote waiting'

foreach ($path in @('tools/github_post_push_audit.ps1', 'tools/test_github_post_push_audit.ps1',
        'tools/scanner_metadata_review.py', 'tools/test_scanner_metadata_review.py', '.github/scanner-source-reviews.csv',
        'tools/scanner_hosted_review.py', 'tools/test_scanner_hosted_review.py', '.github/scanner-hosted-reviews.csv', '.github/scanner-hosted-approval.csv')) {
    $auditPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    if ($path.StartsWith('tools/')) {
        Assert-Selector $auditPlan.scope.touchesVerification "post-push audit changes are verification tooling: $path"
    }
    Assert-Selector (-not $auditPlan.scope.fullEscalationRequired) "post-push audit changes must target API failure contracts: $path"
    Assert-Selector (Test-RequiredCommand -Plan $auditPlan -Id "github-post-push-audit-tests") "post-push audit changes must run their offline regressions: $path"
}

foreach ($path in @('tools/scanner_hosted_review.py', 'tools/test_scanner_hosted_review.py',
        '.github/scanner-hosted-reviews.csv', '.github/scanner-hosted-approval.csv', 'tools/scanner_metadata_review.py', 'tools/github_post_push_audit.ps1')) {
    $hostedReviewPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $hostedReviewPlan -Id 'scanner-hosted-review-tests') "hosted review producers and consumers must run expiry contracts: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $hostedReviewPlan -Id 'release-build')) "hosted admission tooling must not repeat an unchanged native build: $path"
}

$unknownScope = Get-SuperZipVerificationScope -ChangedPath @('unexpected/new-area.file')
Assert-Selector $unknownScope.requiresClassificationReview 'unknown paths require classification before execution'
Assert-Selector (-not $unknownScope.fullEscalationRequired) 'unknown paths are not evidence that unrelated checks are relevant'
$unknownRejected = $false
try { Get-SuperZipVerificationPlan -ChangedPath @('unexpected/new-area.file') | Out-Null }
catch { $unknownRejected = $_.Exception.Message -match 'Classify these changed paths' }
Assert-Selector $unknownRejected 'unknown paths must fail planning rather than silently omit checks'
$manyDocs = @(1..30 | ForEach-Object { "docs/fixture-$_.md" })
$manyDocsPlan = Get-SuperZipVerificationPlan -ChangedPath $manyDocs
Assert-Selector (-not $manyDocsPlan.scope.fullEscalationRequired) 'path count alone must not turn a documentation batch into product testing'
Assert-Selector ($manyDocsPlan.requiredLocalCommands.Count -eq 3) 'documentation batches retain hygiene, actual scanner admission and language lint'

$forcedPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/targeted-verification.md") -SuspectGlobalBug
Assert-Selector $forcedPlan.scope.fullEscalationRequired "SuspectGlobalBug must escalate even for docs"
Assert-Selector (Test-RequiredCommand -Plan $forcedPlan -Id "security-scan") "forced full profile must include security scan"
Assert-Selector (Test-RequiredCommand -Plan $forcedPlan -Id "benchmark-reporting-test") "full verification must cover typed benchmark reporting"
$benchmarkPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/bench.ps1")
Assert-Selector (Test-RequiredCommand -Plan $benchmarkPlan -Id "benchmark-reporting-test") "benchmark changes must test reporting without running timing workloads"
$benchmarkCommand = @($benchmarkPlan.manualLocalCommands | Where-Object { $_.id -eq 'ram-benchmark-sweep' })
Assert-Selector ($benchmarkCommand.Count -eq 1 -and $benchmarkCommand[0].arguments[3] -eq '-Command' -and
    $benchmarkCommand[0].arguments[4] -match '-BlockSizeKiB 256,512,1024,2048,4096,8192,16384$') `
    'benchmark sweep must use PowerShell expression binding for its integer array'
$arrayProbe = '& { param([int[]]$BlockSizeKiB) $expected = @(256,512,1024,2048,4096,8192,16384); if ($BlockSizeKiB.Count -ne $expected.Count) { exit 7 }; for ($i = 0; $i -lt $expected.Count; $i++) { if ($BlockSizeKiB[$i] -ne $expected[$i]) { exit 7 } } } -BlockSizeKiB 256,512,1024,2048,4096,8192,16384'
& powershell -NoProfile -Command $arrayProbe | Out-Null
Assert-Selector ($LASTEXITCODE -eq 0) 'benchmark sweep integer array must bind as seven values under powershell -Command'

foreach ($mode in @('defer', 'opportunistic')) {
    Assert-Selector ((Invoke-WaiterSmoke -Arguments @('-ChangedPath', 'README.md', '-Mode', $mode, '-FinalCommit')) -ne 0) `
        'final commit must reject every nonblocking mode before accessing GitHub'
}
Assert-Selector ((Invoke-WaiterSmoke -Arguments @('-ChangedPath', 'README.md', '-FinalCommit', '-SkipPostPushAudit')) -ne 0) `
    'final commit must reject the legacy audit-skip switch'
$parseErrors = $null
$waiterAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'wait_relevant_workflows.ps1'), [ref]$null, [ref]$parseErrors)
Assert-Selector ($parseErrors.Count -eq 0) 'waiter functions must parse before isolated tests'
foreach ($definition in $waiterAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
$olderCancelled = [pscustomobject]@{
    databaseId = 100; workflowName = "lint"; status = "completed"; conclusion = "cancelled"; url = "https://example.test/100"
}
$newerRunning = [pscustomobject]@{
    databaseId = 101; workflowName = "lint"; status = "in_progress"; conclusion = ""; url = "https://example.test/101"
}
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($olderCancelled, $newerRunning) -WorkflowName @("lint")
Assert-Selector ($duplicateStatus.failed.Count -eq 0 -and $duplicateStatus.running.Count -eq 1) "newer active run must supersede older cancelled run"
$newerRunning.status = "completed"
$newerRunning.conclusion = "success"
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($olderCancelled, $newerRunning) -WorkflowName @("lint")
Assert-Selector $duplicateStatus.complete "newer successful run must supersede older cancelled run"
$newerRunning.conclusion = "failure"
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($newerRunning, $olderCancelled) -WorkflowName @("lint")
Assert-Selector ($duplicateStatus.failed.Count -eq 1) "newer failed run must not be hidden by response ordering"
$newerCancelled = [pscustomobject]@{
    databaseId = 102; workflowName = "lint"; status = "completed"; conclusion = "cancelled"; url = "https://example.test/102"
}
$newerRunning.status = "in_progress"
$newerRunning.conclusion = ""
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($newerCancelled, $newerRunning) -WorkflowName @("lint")
Assert-Selector ($duplicateStatus.failed.Count -eq 0 -and $duplicateStatus.running.Count -eq 1) "newer cancelled duplicate must not hide an active same-commit run"
$newerRunning.status = "completed"
$newerRunning.conclusion = "success"
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($newerCancelled, $newerRunning) -WorkflowName @("lint")
Assert-Selector $duplicateStatus.complete "newer cancelled duplicate must not hide a successful same-commit run"
$duplicateStatus = Test-SelectedWorkflowCompletion -Runs @($newerCancelled, $olderCancelled) -WorkflowName @("lint")
Assert-Selector ($duplicateStatus.failed.Count -eq 1) "all-cancelled workflow runs must still fail closed"

$corpusPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/benchmark_corpus.py', 'tools/benchmark_corpus.ps1', 'tools/test_benchmark_corpus.py')
Assert-Selector (($corpusPlan.requiredLocalCommands.id -contains 'benchmark-corpus-admission-test') -and
    ($corpusPlan.requiredLocalCommands.id -contains 'benchmark-reporting-test') -and
    ($corpusPlan.requiredLocalCommands.id -notcontains 'unit-tests') -and
    ($corpusPlan.requiredLocalCommands.id -notcontains 'benchmark-tooling-tests')) 'corpus admission changes must select their own contracts and controller, without unrelated native or graph suites'

$acquisitionPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/acquire_benchmark_corpus.py', 'tools/test_acquire_benchmark_corpus.py')
Assert-Selector (($acquisitionPlan.requiredLocalCommands.id -contains 'benchmark-corpus-acquisition-test') -and
    ($acquisitionPlan.requiredLocalCommands.id -notcontains 'unit-tests') -and
    ($acquisitionPlan.requiredLocalCommands.id -notcontains 'release-build')) 'corpus acquisition must select its admission and publication contracts without unrelated product builds or native tests'

$binaryPlan = Get-SuperZipVerificationPlan -ChangedPath @('tools/test_memory_benchmark_corpus.ps1')
Assert-Selector (($binaryPlan.requiredLocalCommands.id -contains 'binary-corpus-transport-tests') -and
    ($binaryPlan.requiredLocalCommands.id -notcontains 'release-build') -and
    ($binaryPlan.requiredLocalCommands.id -notcontains 'unit-tests')) 'binary adapter changes must invoke their actual offline consumer without unrelated native work'

Write-Output "Verification selector self-test passed."
