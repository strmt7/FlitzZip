# Purpose: Route hosted compilation to actual CPU and HIP source boundaries.
# Inputs: Paths are normalized changes; Full explicitly admits both compile qualifications.
# Outputs: Returns CPU/HIP workflow admission without substituting CPU tests for device compilation.
function Get-SuperZipHostedWorkflowSelection {
    param([string[]]$Paths, [switch]$Full)
    $cpuPaths = @($Paths | Where-Object { $_ -notmatch '^src/gpu/[^/]+\.hip\.cpp$' })
    $cpu = Get-SuperZipVerificationScope -ChangedPath $cpuPaths
    $windows = $Full.IsPresent -or $cpu.touchesCpp -or $cpu.touchesProductionSource -or
        $cpu.touchesGui -or $cpu.touchesPackaging -or $cpu.touchesNativeBuildInputs -or
        (Test-SuperZipAnyPath -Path $Paths -Pattern @('^\.github/workflows/windows-ci\.yml$'))
    $rocm = $Full.IsPresent -or (Test-SuperZipAnyPath -Path $Paths -Pattern @(
        '^\.github/workflows/rocm-qualification\.yml$', '^CMakeLists\.txt$', '^cmake/',
        '^src/gpu/', '^src/core/[^/]+\.hpp$', '^third_party/',
        '^tools/(build|compile_hip_object|hip_architecture|rocm_toolchain|process_environment|cmake_toolchain|build_parallelism|local_resources)\.ps1$',
        '^tools/(bootstrap_rocm_sdk|native_build_provenance|native_build_receipt)\.py$', '^tools/rocm-sdk-lock\.json$',
        '^tools/cmake-toolchain\.sha256$'
    ))
    return [pscustomobject]@{ windows = $windows; rocm = $rocm }
}

# Purpose: Project hosted native work from the canonical planner instead of repeating every product check.
# Inputs: Paths describe the push/PR range; Full is explicit manual broad CPU qualification.
# Outputs: Returns build/test/matrix decisions and the trusted native command; never launches a workload.
function Get-SuperZipHostedNativePlan {
    param([string[]]$Paths, [switch]$Full)
    $plan = Get-SuperZipVerificationPlan -ChangedPath $Paths -SuspectGlobalBug:$Full -Checkpoint intermediate
    $unit = @($plan.requiredLocalCommands | Where-Object id -eq 'unit-tests')
    $mode = 'none'
    $command = $null
    if ($unit.Count) {
        if ($unit.Count -ne 1) { throw 'Hosted native plan requires exactly one native test command.' }
        $command = $unit[0]
        $mode = if ('tools/native_component_tests.ps1' -in $command.arguments) { 'component' } else { 'full' }
    }
    return [pscustomobject]@{
        build = @($plan.requiredLocalCommands | Where-Object id -eq 'release-build').Count -gt 0
        testMode = $mode; testCommand = $command
        formatMatrix = @($plan.requiredLocalCommands | Where-Object id -eq 'format-matrix-smoke').Count -gt 0
        securityPolicy = @($plan.requiredLocalCommands | Where-Object id -eq 'security-scan').Count -gt 0
        gpuExecutionQualified = $false
    }
}

# Purpose: Resolve a complete trusted Git event range without accepting shell expressions as revisions.
# Inputs: EventName and complete event SHAs; manual runs are broad unless DispatchBase selects a reviewed range.
# Outputs: Returns changed paths and broad intent; rejects missing/malformed bases or unavailable Git evidence.
function Get-SuperZipNativeCiEvent {
    param([ValidateSet('push', 'pull_request', 'workflow_dispatch')][string]$EventName,
          [string]$PushBase, [string]$PullRequestBase, [string]$DispatchBase)
    if ($DispatchBase -and $EventName -ne 'workflow_dispatch') { throw 'A manual comparison base requires workflow_dispatch.' }
    if ($EventName -eq 'workflow_dispatch' -and -not $DispatchBase) { return [pscustomobject]@{ paths = @('README.md'); full = $true } }
    $base = if ($EventName -eq 'pull_request') { $PullRequestBase }
        elseif ($EventName -eq 'workflow_dispatch') { $DispatchBase } else { $PushBase }
    if ($base -notmatch '^[0-9a-f]{40}$' -or ($EventName -ne 'push' -and $base -match '^0{40}$')) {
        throw 'Hosted native selection requires a valid complete event base SHA.'
    }
    if ($base -match '^0{40}$') {
        $paths = @(git ls-files)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate initial native event inputs.' }
    } else {
        $paths = @(Get-SuperZipChangedPath -BaseRef $base -HeadRef HEAD)
    }
    return [pscustomobject]@{ paths = $paths; full = $false }
}

# Purpose: Execute only the hosted CPU native command selected from the actual change range.
# Inputs: Plan is canonical projection; component commands receive explicit CPU-only validation.
# Outputs: Runs selected tests once or reports none; hardware qualification always remains false.
function Invoke-SuperZipHostedNativeTest {
    param([Parameter(Mandatory = $true)]$Plan)
    if ($Plan.testMode -eq 'none') { Write-Output 'No native runtime tests selected; affected mechanism contracts run in the component lane.'; return }
    if (-not $Plan.build -or $Plan.testMode -notin @('component', 'full') -or $Plan.testCommand.id -ne 'unit-tests') {
        throw 'Invalid hosted native execution plan.'
    }
    $command = $Plan.testCommand.PSObject.Copy()
    if ($Plan.testMode -eq 'component') { $command.arguments = @($command.arguments) + '-CpuOnlyValidation' }
    Write-Output 'Hosted CPU-only validation: GPU execution is not qualified.'
    Invoke-SuperZipVerificationCommand -Command $command
}
