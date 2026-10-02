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
Assert-Selector ($emptyPaths.Count -eq 0) "clean git output must normalize to an empty path set without binding errors"

foreach ($path in @('tools/fuzz_resources.ps1', 'tools/test_fuzz_resources.ps1',
        'tools/fuzz_memory.py', 'tools/test_fuzz_memory.py')) {
    $resourcePlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector $resourcePlan.scope.fullEscalationRequired 'fuzz resource boundaries must receive full verification'
    Assert-Selector (Test-RequiredCommand -Plan $resourcePlan -Id 'fuzz-resource-tests') 'fuzz admission tests must remain selected'
    Assert-Selector (Test-RequiredCommand -Plan $resourcePlan -Id 'fuzz-memory-tests') 'cgroup limit regressions must remain selected'
    Assert-Selector (Test-RequiredCommand -Plan $resourcePlan -Id 'short-fuzz-smoke') 'resource changes must exercise real sanitizer targets'
}

$docsPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/targeted-verification.md")
Assert-Selector $docsPlan.scope.docsOnly "docs-only changes must be classified as docsOnly"
Assert-Selector (-not $docsPlan.scope.fullEscalationRequired) "docs-only changes must not escalate"
Assert-Selector ((Get-RequiredCommandId -Plan $docsPlan).Count -eq 2) "docs-only changes must require changed hygiene and language lint"
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

$gpuPlan = Get-SuperZipVerificationPlan -ChangedPath @("src/gpu/dictionary_candidate.cpp")
Assert-Selector $gpuPlan.scope.touchesPerformance "GPU codec changes must retain performance verification"
Assert-Selector (-not (Test-Workflow -Plan $gpuPlan -Name "benchmark-graph")) "GPU source alone must not wait for a path-filtered graph workflow"

$archivePlan = Get-SuperZipVerificationPlan -ChangedPath @("src/zip/zip_adapter.cpp")
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "security-scan") "archive parser changes must run security scan"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "compatibility-interop-smoke") "archive parser changes must run external compatibility interop smoke"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "format-matrix-smoke") "archive parser changes must run the registry-wide format matrix smoke"
Assert-Selector (Test-RequiredCommand -Plan $archivePlan -Id "short-fuzz-smoke") "archive parser changes must run short fuzz smoke"
Assert-Selector (-not (Test-Workflow -Plan $archivePlan -Name "fuzzing")) "archive parser changes must not block normal waits on fuzzing"
Assert-Selector (Test-LongRunningWorkflow -Plan $archivePlan -Name "fuzzing") "archive parser changes must still observe fuzzing as a long-running workflow"

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

foreach ($path in @(".clusterfuzzlite/build.sh", ".clusterfuzzlite/local_smoke.sh", ".clusterfuzzlite/Dockerfile", ".clusterfuzzlite/project.yaml", "tools/test_verification_selector.ps1", "tools/build_parallelism.ps1", "tools/test_build_parallelism.ps1", "tools/refactor_audit.ps1", "tools/test_refactor_audit.ps1")) {
    $buildGraphPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector $buildGraphPlan.scope.touchesVerification "independent build graph and verifier tests must be classified as verification tooling: $path"
    Assert-Selector $buildGraphPlan.scope.fullEscalationRequired "verification build inputs must escalate: $path"
    Assert-Selector (Test-RequiredCommand -Plan $buildGraphPlan -Id "verification-selector-self-test") "verification build inputs must test the selector: $path"
    Assert-Selector (Test-RequiredCommand -Plan $buildGraphPlan -Id "refactor-audit-tests") "verification build inputs must test the source inventory: $path"
    Assert-Selector (Test-RequiredCommand -Plan $buildGraphPlan -Id "build-parallelism-test") "verification build inputs must test bounded scheduling: $path"
    Assert-Selector (Test-LongRunningWorkflow -Plan $buildGraphPlan -Name "fuzzing") "verification build inputs must observe Linux build and fuzzing: $path"
    Assert-Selector $buildGraphPlan.workflowWaitPolicy.immediateRequired "verification build inputs must require final workflow waiting: $path"
}

$workflowPlan = Get-SuperZipVerificationPlan -ChangedPath @(".github/workflows/security-code-scanning.yml")
foreach ($path in @('tools/rocm_toolchain.ps1', 'tools/bootstrap_rocm_sdk.py', 'tools/test_bootstrap_rocm_sdk.py',
        'tools/test_rocm_toolchain.ps1', 'tools/rocm-sdk-lock.json', 'tools/compile_hip_object.ps1')) {
    $rocmPlan = Get-SuperZipVerificationPlan -ChangedPath @($path) -Checkpoint intermediate
    Assert-Selector $rocmPlan.scope.touchesVerification "ROCm build/provisioning inputs require compiler and verifier coverage: $path"
    Assert-Selector (Test-RequiredCommand -Plan $rocmPlan -Id 'rocm-bootstrap-tests') "ROCm provisioning needs offline preservation and archive-boundary tests: $path"
    Assert-Selector (Test-RequiredCommand -Plan $rocmPlan -Id 'release-build') "ROCm changes must rebuild the HIP product: $path"
}
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "security-scan") "workflow changes must run security scan"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "lint") "workflow changes must wait for lint"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "benchmark-graph") "workflow changes must wait for benchmark graph validation"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "security") "workflow changes must wait for security"
Assert-Selector (Test-Workflow -Plan $workflowPlan -Name "scorecard") "workflow changes must wait for scorecard"
Assert-Selector $workflowPlan.postPushAuditRequired "workflow changes must require post-push audit"
$benchmarkPlan = Get-SuperZipVerificationPlan -ChangedPath @("docs/benchmarks/data/effort-native-L5.json")
Assert-Selector (Test-Workflow -Plan $benchmarkPlan -Name "benchmark-graph") "benchmark records must wait for graph regeneration"
foreach ($path in @("tools/benchmark_cache.py", "tools/test_benchmark_cache.py", "tools/benchmark_comparators.py", "tools/test_benchmark_comparators.py", "tools/benchmark_permissions.json", "docs/benchmark-permissions.md", "docs/benchmark-research.md", "docs/comparative-benchmark-methodology.md")) {
    $benchmarkToolPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "benchmark-tooling-tests") "benchmark tools and policies require offline validation: $path"
    Assert-Selector (Test-Workflow -Plan $benchmarkToolPlan -Name "benchmark-graph") "benchmark tooling changes must select graph validation: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "release-build")) "benchmark tooling alone must not rebuild the application: $path"
    Assert-Selector (-not (Test-RequiredCommand -Plan $benchmarkToolPlan -Id "gui-smoke")) "benchmark tooling alone must not launch the GUI: $path"
}
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "secret-report-tests") "workflow changes must test secret artifact redaction"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "scanner-coverage-tests") "workflow changes must test complete scanner coverage evidence"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "devskim-provenance-tests") "workflow changes must test exact scanner package provenance"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "devskim-report-tests") "workflow changes must test lossless scanner report publication"
Assert-Selector (Test-RequiredCommand -Plan $workflowPlan -Id "greenbone-config-tests") "workflow changes must test broker authorization and masking"

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

foreach ($path in @("tools/redact_trufflehog.py", "tools/test_redact_trufflehog.py", "tools/scan_trufflehog.sh")) {
    $redactionPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector $redactionPlan.scope.fullEscalationRequired "secret report publication changes must escalate: $path"
    Assert-Selector (Test-RequiredCommand -Plan $redactionPlan -Id "secret-report-tests") "redaction changes must execute their regressions: $path"
    Assert-Selector (Test-Workflow -Plan $redactionPlan -Name "security") "redaction changes must require hosted scanner validation: $path"
}

$packagingPlan = Get-SuperZipVerificationPlan -ChangedPath @("CMakeLists.txt")
Assert-Selector (Test-RequiredCommand -Plan $packagingPlan -Id "msi-identity-smoke") "packaging changes must run MSI identity smoke"
Assert-Selector (Test-RequiredCommand -Plan $packagingPlan -Id "package-smoke") "packaging changes must run package smoke"
Assert-Selector (Test-Workflow -Plan $packagingPlan -Name "windows-ci") "packaging changes must wait for windows-ci"

$mcpPlan = Get-SuperZipVerificationPlan -ChangedPath @("mcp/superzip_mcp.py")
foreach ($agentToolPath in @("tools/agent_context.py", "tools/test_agent_context.py", "tools/cocoindex_agent_search.py", "tools/test_cocoindex_agent_search.py")) {
    $agentToolPlan = Get-SuperZipVerificationPlan -ChangedPath @($agentToolPath)
    Assert-Selector $agentToolPlan.scope.fullEscalationRequired "agent context and routing changes must escalate"
    Assert-Selector (Test-RequiredCommand -Plan $agentToolPlan -Id "agent-context-contracts") "agent tool changes must exercise real context and search receipt contracts"
    Assert-Selector (Test-RequiredCommand -Plan $agentToolPlan -Id "verification-selector-self-test") "agent tool routing must retain selector coverage"
}
Assert-Selector $mcpPlan.scope.fullEscalationRequired "MCP verifier-adjacent changes must escalate"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "mcp-python-compile") "MCP changes must compile Python"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "mcp-bounded-child-tests") "MCP changes must test bounded child execution"
Assert-Selector (Test-RequiredCommand -Plan $mcpPlan -Id "verification-selector-self-test") "MCP/verifier changes must self-test selector"

$verifierPlan = Get-SuperZipVerificationPlan -ChangedPath @("tools/superzip_verification.psm1")
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
        'mcp/superzip_mcp.py', '.agents/skills/superzip-build-test/SKILL.md', 'tools/superzip_verification.psm1', 'unexpected/new-area.file')) {
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

foreach ($path in @("tools/github_post_push_audit.ps1", "tools/test_github_post_push_audit.ps1")) {
    $auditPlan = Get-SuperZipVerificationPlan -ChangedPath @($path)
    Assert-Selector $auditPlan.scope.touchesVerification "post-push audit changes are verification tooling: $path"
    Assert-Selector $auditPlan.scope.fullEscalationRequired "post-push audit changes must escalate: $path"
    Assert-Selector (Test-RequiredCommand -Plan $auditPlan -Id "github-post-push-audit-tests") "post-push audit changes must run their offline regressions: $path"
}

$unknownPlan = Get-SuperZipVerificationPlan -ChangedPath @("unexpected/new-area.file")
Assert-Selector $unknownPlan.scope.fullEscalationRequired "unknown paths must escalate"
Assert-Selector ($unknownPlan.scope.unknownPaths.Count -eq 1) "unknown path must be reported"

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

Write-Output "Verification selector self-test passed."
