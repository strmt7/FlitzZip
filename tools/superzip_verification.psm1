$Script:SuperZipVerificationRepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'local_resources.ps1')

# Purpose: Convert a path to the repository-relative slash form used by the verification classifier.
# Inputs: `Path` may be absolute, relative, slash-separated, or backslash-separated.
# Outputs: Returns a normalized relative path string.
function ConvertTo-SuperZipRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $trimmed = $Path.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return ""
    }
    $normalized = $trimmed -replace "\\", "/"
    $repo = ($Script:SuperZipVerificationRepoRoot -replace "\\", "/").TrimEnd("/")
    if ([System.IO.Path]::IsPathRooted($trimmed)) {
        $full = ([System.IO.Path]::GetFullPath($trimmed) -replace "\\", "/")
        if ($full.StartsWith($repo + "/", [System.StringComparison]::OrdinalIgnoreCase)) {
            $normalized = $full.Substring($repo.Length + 1)
        } else {
            $normalized = $full
        }
    }
    while ($normalized.StartsWith("./", [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    return $normalized.Trim("/")
}

# Purpose: Return a stable unique path list from arbitrary path text.
# Inputs: `Path` contains raw path strings that may include empty values or duplicates.
# Outputs: Returns sorted repository-relative path strings.
function Select-SuperZipUniquePath {
    param([string[]]$Path)

    $set = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($item in @($Path)) {
        if ([string]::IsNullOrWhiteSpace($item)) {
            continue
        }
        $relative = ConvertTo-SuperZipRelativePath -Path $item
        if (-not [string]::IsNullOrWhiteSpace($relative)) {
            [void]$set.Add($relative)
        }
    }
    return @($set | Sort-Object)
}

# Purpose: Resolve changed paths from explicit input or git diff state.
# Inputs: `ChangedPath` overrides discovery; `BaseRef`/`HeadRef` select a git range; `IncludeUntracked` includes untracked local files.
# Outputs: Returns sorted repository-relative changed paths.
function Get-SuperZipChangedPath {
    param(
        [string[]]$ChangedPath = @(),
        [string]$BaseRef = "",
        [string]$HeadRef = "HEAD",
        [switch]$IncludeUntracked
    )

    if (@($ChangedPath).Count -gt 0) {
        return Select-SuperZipUniquePath -Path $ChangedPath
    }

    $paths = @()
    Push-Location $Script:SuperZipVerificationRepoRoot
    try {
        if (-not [string]::IsNullOrWhiteSpace($BaseRef)) {
            $gitArgs = @("diff", "--name-only", "--diff-filter=ACDMRTUX", $BaseRef)
            if (-not [string]::IsNullOrWhiteSpace($HeadRef)) {
                $gitArgs += $HeadRef
            }
            $paths += & git @gitArgs
        } else {
            $paths += & git diff --name-only --diff-filter=ACDMRTUX
            $paths += & git diff --cached --name-only --diff-filter=ACDMRTUX
            if ($IncludeUntracked.IsPresent) {
                $paths += & git ls-files --others --exclude-standard
            }
        }
    } finally {
        Pop-Location
    }
    return Select-SuperZipUniquePath -Path $paths
}

# Purpose: Test whether any changed path matches one of the supplied regular expressions.
# Inputs: `Path` is the normalized path set and `Pattern` contains anchored regular expressions.
# Outputs: Returns true when at least one path matches.
function Test-SuperZipAnyPath {
    param(
        [string[]]$Path,
        [string[]]$Pattern
    )

    foreach ($pathItem in @($Path)) {
        foreach ($patternItem in @($Pattern)) {
            if ($pathItem -match $patternItem) {
                return $true
            }
        }
    }
    return $false
}

# Purpose: Test whether every changed path matches at least one supplied regular expression.
# Inputs: `Path` is the normalized path set and `Pattern` contains anchored regular expressions.
# Outputs: Returns true for an empty path set or when all paths are classified.
function Test-SuperZipAllPath {
    param(
        [string[]]$Path,
        [string[]]$Pattern
    )

    foreach ($pathItem in @($Path)) {
        $matched = $false
        foreach ($patternItem in @($Pattern)) {
            if ($pathItem -match $patternItem) {
                $matched = $true
                break
            }
        }
        if (-not $matched) {
            return $false
        }
    }
    return $true
}

# Purpose: Build one executable verification command descriptor.
# Inputs: Command metadata, executable name, argv, and rationale.
# Outputs: Returns an ordered object suitable for JSON output and execution.
function Get-SuperZipVerificationCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Reason,
        [ValidateSet("required", "manual")][string]$Requirement = "required"
    )

    $quoted = @($Executable) + @($Arguments) | ForEach-Object {
        if ($_ -match '\s') {
            '"' + ($_ -replace '"', '\"') + '"'
        } else {
            $_
        }
    }
    return [pscustomobject][ordered]@{
        id = $Id
        stage = $Stage
        requirement = $Requirement
        executable = $Executable
        arguments = @($Arguments)
        command = ($quoted -join " ")
        reason = $Reason
    }
}

# Purpose: Add a command to a mutable list only once by command id.
# Inputs: `List` is an ArrayList, `Seen` is a HashSet of ids, and `Command` is a command descriptor.
# Outputs: Mutates `List` and `Seen` when the command id is new.
function Add-SuperZipVerificationCommand {
    param(
        [Parameter(Mandatory = $true)]$List,
        [Parameter(Mandatory = $true)]$Seen,
        [Parameter(Mandatory = $true)]$Command
    )

    if ($Seen.Add([string]$Command.id)) {
        [void]$List.Add($Command)
    }
}

# Purpose: Detect whether the current git diff for selected files changes performance-sensitive text.
# Inputs: `Path` is the normalized path set and `Pattern` contains regexes for benchmark/performance terms.
# Outputs: Returns true when a staged or unstaged added/removed line matches; returns false when no matching diff is available.
function Test-SuperZipDiffLine {
    param(
        [string[]]$Path,
        [string[]]$Pattern
    )

    $candidatePaths = @($Path | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($candidatePaths.Count -eq 0) {
        return $false
    }

    Push-Location $Script:SuperZipVerificationRepoRoot
    try {
        foreach ($pathItem in $candidatePaths) {
            $diffLines = @()
            $diffLines += & git diff --unified=0 -- $pathItem
            $diffLines += & git diff --cached --unified=0 -- $pathItem
            foreach ($line in $diffLines) {
                if ($line -notmatch '^[+-]' -or $line -match '^(\+\+\+|---)') {
                    continue
                }
                foreach ($patternItem in @($Pattern)) {
                    if ($line -match $patternItem) {
                        return $true
                    }
                }
            }
        }
    } finally {
        Pop-Location
    }
    return $false
}

# Purpose: Classify changed paths into verification-relevant SuperZip risk areas.
# Inputs: `ChangedPath` is a normalized path set and `SuspectGlobalBug` forces full escalation.
# Outputs: Returns a scope object with booleans, unknown paths, and escalation reasons.
function Get-SuperZipVerificationScope {
    param(
        [string[]]$ChangedPath,
        [switch]$SuspectGlobalBug
    )

    $paths = Select-SuperZipUniquePath -Path $ChangedPath
    $documentationPatterns = @(
        '^AGENTS\.md$',
        '^README\.md$',
        '^IMPLEMENTATION_PLAN\.md$',
        '^docs/',
        '^\.github/copilot-instructions\.md$',
        '^\.agents/skills/.*\.md$'
    )
    $knownPatterns = @(
        '^AGENTS\.md$',
        '^README\.md$',
        '^IMPLEMENTATION_PLAN\.md$',
        '^LICENSE(\.md)?$',
        '^CMakeLists\.txt$',
        '^cmake/',
        '^docs/',
        '^src/',
        '^tests/',
        '^fuzz/',
        '^tools/',
        '^mcp/',
        '^\.agents/',
        '^\.github/',
        '^\.clang-format$',
        '^\.pymarkdown\.json$',
        '^\.ruff\.toml$',
        '^\.yamllint$',
        '^\.clusterfuzzlite/',
        '^resources/',
        '^third_party/'
    )
    $unknown = @()
    foreach ($path in $paths) {
        if (-not (Test-SuperZipAnyPath -Path @($path) -Pattern $knownPatterns)) {
            $unknown += $path
        }
    }

    $touchesWorkflow = Test-SuperZipAnyPath -Path $paths -Pattern @('^\.github/(workflows|actions|codeql|requirements|openvas)/', '^\.github/dependabot\.yml$')
    $touchesVerification = Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^tools/(superzip_verification\.psm1|test_verification_selector\.ps1|verification_plan\.ps1|verify_changes\.ps1|verify_change_hygiene\.ps1|wait_relevant_workflows\.ps1|security_scan\.ps1|github_post_push_audit\.ps1|refactor_audit\.ps1|format_matrix_smoke\.ps1|test_msi_identity\.ps1|test\.ps1|build\.ps1|fuzz\.ps1)$',
        '^tools/(redact_trufflehog|test_redact_trufflehog)\.py$',
        '^tools/scan_trufflehog\.sh$',
        '^tools/(build_parallelism|test_build_parallelism|local_resources|test_workflow_checkpoint)\.ps1$',
        '^tools/(rocm_toolchain|test_rocm_toolchain|compile_hip_object|hip_architecture|test_hip_architecture)\.ps1$',
        '^tools/(bootstrap_rocm_sdk|test_bootstrap_rocm_sdk)\.py$',
        '^tools/rocm-sdk-lock\.json$',
        '^tools/(fuzz_resources|test_fuzz_resources)\.ps1$',
        '^tools/(fuzz_memory|test_fuzz_memory)\.py$',
        '^tools/(agent_context|test_agent_context|cocoindex_agent_search|test_cocoindex_agent_search)\.py$',
        '^tools/test_github_post_push_audit\.ps1$',
        '^tools/test_refactor_audit\.ps1$',
        '^\.clusterfuzzlite/(build\.sh|local_smoke\.sh|Dockerfile|project\.yaml)$',
        '^mcp/',
        '^\.agents/skills/'
    )
    $touchesCpp = Test-SuperZipAnyPath -Path $paths -Pattern @('^(src|tests|fuzz)/.*\.(cpp|hpp|c|h|rc)$', '^CMakeLists\.txt$', '^cmake/', '^third_party/(?!upstream/)')
    $touchesProductionSource = Test-SuperZipAnyPath -Path $paths -Pattern @('^src/', '^CMakeLists\.txt$', '^cmake/', '^third_party/(?!upstream/)')
    $touchesArchiveParser = Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^src/(ar|arc|arj|base64|bzip2|cab|cpio|gzip|hqx|iso|lha|lzip|lzma|macbinary|rpm|sevenzip|tar|unix_compress|uue|wim|xar|xxe|xz|zip|zstd)/',
        '^src/core/(archive|archive_format|archive_index|archive_name_encoding|file_manifest|file_publish|path_safety|result|progress)\.',
        '^tests/cpp/test_.*(compat|archive|path|format).*\.cpp$',
        '^fuzz/'
    )
    $touchesSecurityBoundary = $touchesArchiveParser -or (Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^src/core/(defender_scan|integrity|path_safety|file_publish)\.',
        '^tools/(security_scan|github_post_push_audit|verify_change_hygiene|wait_relevant_workflows)\.ps1$',
        '^tools/(test_semgrep_installation|test_semgrep_runtime|semgrep_coverage|test_semgrep_coverage|devskim_provenance|test_devskim_provenance|devskim_report|test_devskim_report)\.py$',
        '^\.github/'
    ))
    $touchesGui = Test-SuperZipAnyPath -Path $paths -Pattern @('^src/app/', '^resources/(design|app|brand)/', '^tools/(gui_smoke|generate_app_icon|generate_brand_logo_header|verify_brand_assets)\.ps1$', '^tools/SuperZip\.GuiSmoke\.[^/]+\.psm1$')
    $touchesBrand = Test-SuperZipAnyPath -Path $paths -Pattern @('^resources/brand/', '^resources/app/', '^tools/(generate_app_icon|generate_brand_logo_header|verify_brand_assets)\.ps1$', '^src/app/superzip_brand_logo')
    $touchesPackaging = Test-SuperZipAnyPath -Path $paths -Pattern @('^CMakeLists\.txt$', '^cmake/', '^tools/(package|install_wix|build|version|release_metadata)\.(ps1|py)$', '^\.github/actions/windows-release/', '^\.github/workflows/release\.yml$')
    $touchesLintSurface = Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^\.clang-format$',
        '^\.github/.*\.ya?ml$',
        '^\.github/workflows/lint\.yml$',
        '^\.pymarkdown\.json$',
        '^\.ruff\.toml$',
        '^\.yamllint$',
        '^AGENTS\.md$',
        '^README\.md$',
        '^docs/.*\.md$',
        '^mcp/.*\.py$',
        '^tools/.*\.(ps1|psm1|py)$',
        '^CMakeLists\.txt$',
        '^cmake/',
        '^(src|tests|fuzz)/.*\.(c|cc|cpp|h|hpp)$'
    )
    $touchesBenchmarkCliDiff = (Test-SuperZipAnyPath -Path $paths -Pattern @('^src/cli/main\.cpp$')) -and (Test-SuperZipDiffLine -Path @("src/cli/main.cpp") -Pattern @('benchmark', 'throughput', 'compression.?level', 'block.?size', 'workers', 'inflight', 'gpu'))
    $touchesPerformance = $touchesBenchmarkCliDiff -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^src/gpu/', '^src/core/(archive|archive_blocks|archive_block_types|resource_limits)\.', '^tools/(bench|gpu_|storage_smoke)', '^docs/(performance|compression-level|compression-backend)'))
    $touchesMcp = Test-SuperZipAnyPath -Path $paths -Pattern @('^mcp/.*\.py$')
    $docsOnly = (@($paths).Count -gt 0) -and (Test-SuperZipAllPath -Path $paths -Pattern $documentationPatterns)

    $reasons = @()
    if ($SuspectGlobalBug.IsPresent) { $reasons += "caller marked the codebase as globally suspicious" }
    if (@($paths).Count -ge 25) { $reasons += "changed path count is $(@($paths).Count), which is broad enough to hide cross-component regressions" }
    if (@($unknown).Count -gt 0) { $reasons += "unclassified paths require conservative verification: $($unknown -join ', ')" }
    if ($touchesVerification) { $reasons += "verification tooling, MCP, or agent skills changed, so the verifier must test itself and escalate" }

    return [pscustomobject][ordered]@{
        paths = @($paths)
        pathCount = @($paths).Count
        docsOnly = $docsOnly
        touchesWorkflow = $touchesWorkflow
        touchesVerification = $touchesVerification
        touchesCpp = $touchesCpp
        touchesProductionSource = $touchesProductionSource
        touchesArchiveParser = $touchesArchiveParser
        touchesSecurityBoundary = $touchesSecurityBoundary
        touchesGui = $touchesGui
        touchesBrand = $touchesBrand
        touchesPackaging = $touchesPackaging
        touchesLintSurface = $touchesLintSurface
        touchesPerformance = $touchesPerformance
        touchesMcp = $touchesMcp
        unknownPaths = @($unknown)
        fullEscalationRequired = (@($reasons).Count -gt 0)
        fullEscalationReasons = @($reasons)
    }
}

# Purpose: Select local checks without changing coverage for remote checkpoint timing.
# Inputs: Scope and Paths describe the classified change; TouchesBenchmarkGraph selects graph tests.
# Outputs: Returns the ordered, deduplicated required local command descriptors.
function Get-SuperZipLocalVerificationCommand {
    param(
        [Parameter(Mandatory = $true)]$Scope,
        [string[]]$Paths,
        [bool]$TouchesBenchmarkGraph
    )
    $local = New-Object System.Collections.ArrayList
    $seen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    $hygieneArguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/verify_change_hygiene.ps1")
    if (@($paths).Count -gt 0) {
        $hygieneArguments += "-ChangedPathBase64"
        $jsonPaths = ConvertTo-Json -InputObject @($paths) -Compress
        $hygieneArguments += [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($jsonPaths))
    }

    Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "changed-hygiene" -Stage "local" -Executable "powershell" -Arguments $hygieneArguments -Reason "always scan changed files for secrets, forbidden workflow deployment keys, generated binaries, and comparison-name policy")

    if ($scope.touchesLintSurface -or $scope.fullEscalationRequired) {
        Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "language-lint" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/lint.ps1", "-CppMode", "Changed", "-IncludeUntracked") -Reason "changed docs, workflow, script, CMake, or C/C++ surfaces require the fast language linter lane")
        if ($scope.fullEscalationRequired -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^tools/(lint|test_lint_routing)\.ps1$', '^tools/superzip_verification\.psm1$'))) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "lint-routing-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_lint_routing.ps1") -Reason "changed/all/configuration lint routing must retain newly added files and cover owned CMake fixtures")
        }
    }

    if ($touchesBenchmarkGraph -or $scope.fullEscalationRequired) {
        $benchmarkTests = @(
            "-3", "-m", "unittest", "tools.test_benchmark_graph",
            "tools.test_archive_comparison", "tools.test_comparison_graph",
            "tools.test_tradeoff_graph", "tools.test_native_tradeoff_graph",
            "tools.test_benchmark_comparators", "tools.test_benchmark_cache"
        )
        Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "benchmark-tooling-tests" -Stage "local" -Executable "py" -Arguments $benchmarkTests -Reason "benchmark permissions, cache identity and graph contracts require offline tests, never a timed workload")
    }

    if (-not ($scope.docsOnly -and -not $scope.fullEscalationRequired)) {
        if ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesGui -or $scope.touchesPackaging -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "release-build" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/build.ps1", "-Configuration", "Release") -Reason "compiled product, CMake, package, GUI, or broad verification changes require a Release build")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "unit-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", "`$ErrorActionPreference = 'Stop'; & ./tools/test.ps1 -Configuration Release; if (Test-Path variable:\LASTEXITCODE) { exit `$LASTEXITCODE }") -Reason "compiled product or shared verification changes require the C++ test harness with CI-equivalent native exit propagation")
        }
        if ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "changed-refactor-audit" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/refactor_audit.ps1", "-ChangedOnly", "-CheckContracts", "-MaxFunctionLines", "120", "-MaxComplexityMarkers", "35", "-FailOnFindings") -Reason "changed source and verification code must stay small, documented, and reviewable")
        }
        if ($scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "verification-selector-self-test" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_verification_selector.ps1") -Reason "verification tooling or full escalation requires classifier scenario self-tests")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "workflow-checkpoint-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_workflow_checkpoint.ps1") -Reason "intermediate observations must fail on real errors, record pending checks, and preserve final acceptance")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "build-parallelism-test" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_build_parallelism.ps1") -Reason "build scheduling must honor explicit job counts and bound default shared-host load")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "rocm-bootstrap-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_bootstrap_rocm_sdk") -Reason "whole SDK provisioning must reject unsafe archives and changed installations without host changes")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "fuzz-resource-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_fuzz_resources.ps1") -Reason "local fuzz admission must fit Windows and Docker-host RAM without CPU caps")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "fuzz-memory-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_fuzz_memory") -Reason "local fuzzing must verify real cgroup RAM and zero-swap limits before compiling")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "github-post-push-audit-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_github_post_push_audit.ps1") -Reason "post-push audits must reject unavailable API evidence, including partial pagination")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "refactor-audit-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_refactor_audit.ps1") -Reason "source audits must include new and changed files without traversing ignored workspace copies")
        }
        if ($scope.touchesPerformance -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "benchmark-reporting-test" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_benchmark_reporting.ps1") -Reason "benchmark reporting must preserve typed results and unavailable counter values without running a timed workload")
        }
        if ($scope.touchesSecurityBoundary -or $scope.touchesWorkflow -or $scope.touchesPackaging -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "security-scan" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/security_scan.ps1") -Reason "security boundaries, workflows, packaging, or verifier changes require repository policy checks")
        }
        if ($scope.fullEscalationRequired -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^tools/(test_semgrep_installation|test_semgrep_runtime)\.py$', '^\.github/requirements/requirements-semgrep-linux\.(in|txt)$', '^\.github/workflows/security-code-scanning\.yml$'))) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "scanner-installation-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_semgrep_installation") -Reason "scanner pins, complete dependency hashes and the normal production install must remain aligned; real Linux runtime and coverage checks remain mandatory in the security workflow")
        }
        if ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^tools/(semgrep_coverage|test_semgrep_coverage)\.py$'))) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "scanner-coverage-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_semgrep_coverage") -Reason "scanner admission must be compared with complete tracked inventory without hiding diagnostics or publishing source snippets")
        }
        if ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^tools/(devskim_provenance|test_devskim_provenance)\.py$'))) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "devskim-provenance-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_devskim_provenance") -Reason "scanner download limits and exact size/hash verification must use one canonical provenance record")
        }
        if ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired -or (Test-SuperZipAnyPath -Path $paths -Pattern @('^tools/(devskim_report|test_devskim_report)\.py$'))) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "devskim-report-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_devskim_report") -Reason "SARIF publication must preserve all findings, repair only empty optional excerpts, and reject unexpected source content")
        }
        if ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "secret-report-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_redact_trufflehog") -Reason "scanner artifacts must retain findings without publishing secrets or identities")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "greenbone-config-tests" -Stage "local" -Executable "node" -Arguments @("--test", ".github/openvas/resolve_config.test.cjs") -Reason "broker configuration must remain bounded, masked, and authorized before publishing step outputs")
        }
        if ($scope.touchesGui -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "gui-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/gui_smoke.ps1", "-Configuration", "Release") -Reason "GUI or broad changes require all tabs, buttons, toggles, dropdowns, drag/drop, and screenshots")
        }
        if ($scope.touchesBrand -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "brand-assets" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/verify_brand_assets.ps1") -Reason "brand assets changed or broad verification requires logo single-source-of-truth validation")
        }
        if ($scope.touchesArchiveParser -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "compatibility-interop-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/compatibility_interop_smoke.ps1", "-Configuration", "Release") -Reason "compatibility archive changes must prove standard container outputs open in independent Windows readers")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "format-matrix-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/format_matrix_smoke.ps1", "-Configuration", "Release") -Reason "archive format, CLI, parser, or broad changes must exercise every registered format's create/extract contract")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "short-fuzz-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/fuzz.ps1", "-Runs", "16") -Reason "archive parser or broad changes require a bounded sanitizer/fuzzer smoke")
        }
        if ($scope.touchesPackaging -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "msi-identity-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_msi_identity.ps1") -Reason "installer identity changes require deterministic ProductCode and stable UpgradeCode coverage")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "package-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/package.ps1", "-Configuration", "Release") -Reason "installer, CPack, versioning, or broad changes require package validation")
        }
        if ($scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "agent-context-contracts" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "tools.test_agent_context", "tools.test_cocoindex_agent_search") -Reason "mandatory skills, bounded source reads, note freshness and real semantic-routing receipts require executable contracts")
        }
        if ($scope.touchesMcp -or $scope.touchesVerification) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "mcp-python-compile" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "py_compile", "mcp/superzip_mcp.py") -Reason "MCP Python changes require syntax validation")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (Get-SuperZipVerificationCommand -Id "mcp-bounded-child-tests" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "unittest", "discover", "-s", "mcp", "-p", "test_superzip_mcp.py") -Reason "MCP changes require output, timeout, and descendant-containment regressions without shadowing the installed SDK")
        }
    }
    return @($local)
}

# Purpose: Separate the timing of remote acceptance from local verification coverage.
# Inputs: Checkpoint is intermediate/final; Scope and workflow counts describe required remote gates.
# Outputs: Returns explicit pending/final wait policy; no test or audit requirement is removed.
function Get-SuperZipWorkflowWaitPolicy {
    param(
        [ValidateSet('intermediate', 'final')][string]$Checkpoint,
        [Parameter(Mandatory = $true)]$Scope,
        [int]$WorkflowCount,
        [int]$LongRunningCount
    )
    $hasRemoteGates = ($WorkflowCount -gt 0 -or $LongRunningCount -gt 0)
    $isIntermediate = ($Checkpoint -eq 'intermediate')
    $reasons = @()
    if ($Scope.touchesWorkflow) { $reasons += 'workflow or scanner configuration changed' }
    if ($Scope.touchesVerification) { $reasons += 'verification tooling, MCP, or agent skills changed' }
    if ($Scope.fullEscalationRequired) { $reasons += 'full verification escalation is required' }
    return [pscustomobject][ordered]@{
        checkpoint = $Checkpoint
        immediateRequired = ($hasRemoteGates -and -not $isIntermediate)
        deferAllowed = $isIntermediate
        recommendedMode = if (-not $hasRemoteGates) { 'none' } elseif ($isIntermediate) { 'opportunistic' } else { 'final' }
        finalRequired = $hasRemoteGates
        longRunningNormallyDeferred = ($LongRunningCount -gt 0 -and $isIntermediate)
        reasons = @($reasons)
    }
}

# Purpose: Build a targeted local and post-push verification plan from changed paths.
# Inputs: ChangedPath or git range describes the change; SuspectGlobalBug forces full local coverage;
#         Checkpoint controls only when remote acceptance blocks development (safe default: final).
# Outputs: Returns scope, required/manual commands, workflows, audit requirements and checkpoint policy.
function Get-SuperZipVerificationPlan {
    param(
        [string[]]$ChangedPath = @(),
        [string]$BaseRef = "",
        [string]$HeadRef = "HEAD",
        [switch]$IncludeUntracked,
        [switch]$SuspectGlobalBug,
        [ValidateSet('intermediate', 'final')][string]$Checkpoint = 'final'
    )
    $paths = Get-SuperZipChangedPath -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef -IncludeUntracked:$IncludeUntracked
    $scope = Get-SuperZipVerificationScope -ChangedPath $paths -SuspectGlobalBug:$SuspectGlobalBug
    $touchesBenchmarkGraph = Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^docs/benchmarks/', '^resources/benchmarks/',
        '^docs/(comparative-benchmark-methodology|benchmark-permissions|benchmark-research)\.md$',
        '^tools/(render_.*graph|test_.*graph|run_archive_comparison|test_archive_comparison|benchmark_comparators|test_benchmark_comparators|benchmark_cache|test_benchmark_cache)\.py$',
        '^tools/benchmark_permissions\.json$'
    )
    $local = @(Get-SuperZipLocalVerificationCommand -Scope $scope -Paths $paths -TouchesBenchmarkGraph $touchesBenchmarkGraph)
    $manual = New-Object System.Collections.ArrayList
    $manualSeen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)

    if ($scope.touchesPerformance -or $scope.fullEscalationRequired) {
        $benchmarkExpression = "& './tools/bench.ps1' -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384"
        Add-SuperZipVerificationCommand -List $manual -Seen $manualSeen -Command (Get-SuperZipVerificationCommand -Id "ram-benchmark-sweep" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $benchmarkExpression) -Reason "performance-sensitive changes need the RAM-only CPU/GPU benchmark sweep before making speed claims" -Requirement "manual")
    }

    $workflows = New-Object System.Collections.ArrayList
    $longRunningWorkflows = New-Object System.Collections.ArrayList
    $workflowSeen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    $longRunningSeen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($pair in @(
        @("lint", ($scope.touchesLintSurface -or $scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("benchmark-graph", ($touchesBenchmarkGraph -or $scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("windows-ci", ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesGui -or $scope.touchesPackaging -or $scope.fullEscalationRequired)),
        @("security", ($scope.touchesSecurityBoundary -or $scope.touchesWorkflow -or $scope.touchesPackaging -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("greenbone-openvas-vulnetix", ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("scorecard", ($scope.touchesWorkflow -or $scope.fullEscalationRequired))
    )) {
        if ($pair[1] -and $workflowSeen.Add([string]$pair[0])) {
            [void]$workflows.Add([string]$pair[0])
        }
    }
    if (($scope.touchesArchiveParser -or $scope.fullEscalationRequired) -and $longRunningSeen.Add("fuzzing")) {
        [void]$longRunningWorkflows.Add("fuzzing")
    }

    $postPushAuditRequired = ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)
    $waitPolicy = Get-SuperZipWorkflowWaitPolicy -Checkpoint $Checkpoint -Scope $scope `
        -WorkflowCount $workflows.Count -LongRunningCount $longRunningWorkflows.Count

    return [pscustomobject][ordered]@{
        scope = $scope
        requiredLocalCommands = @($local)
        manualLocalCommands = @($manual)
        postPushWorkflows = @($workflows)
        longRunningPostPushWorkflows = @($longRunningWorkflows)
        allPostPushWorkflows = @(@($workflows) + @($longRunningWorkflows))
        postPushAuditRequired = $postPushAuditRequired
        workflowWaitPolicy = $waitPolicy
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }
}

# Purpose: Invoke one curated verification command from a plan.
# Inputs: `Command` is produced by `Get-SuperZipVerificationPlan`.
# Outputs: Throws on insufficient RAM or a non-zero exit; correctness children inherit below-normal priority.
function Invoke-SuperZipVerificationCommand {
    param([Parameter(Mandatory = $true)]$Command)

    Push-Location $Script:SuperZipVerificationRepoRoot
    try {
        Write-Output "verification command=$($Command.id) reason=$($Command.reason)"
        if ($Command.id -in @('release-build', 'unit-tests', 'short-fuzz-smoke', 'gui-smoke', 'package-smoke')) {
            $available = Get-SuperZipAvailableMemoryMiB
            $budget = Resolve-SuperZipLocalMemoryBudget -AvailableMiB $available
            Write-Output "local resource admission available_mib=$available budget_mib=$budget"
        }
        if ($Command.id -eq 'ram-benchmark-sweep') {
            & $Command.executable @($Command.arguments)
        } else {
            Invoke-SuperZipBackgroundWork { & $Command.executable @($Command.arguments) }
        }
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "Verification command '$($Command.id)' failed with exit code $exitCode."
        }
    } finally {
        Pop-Location
    }
}

Export-ModuleMember -Function `
    ConvertTo-SuperZipRelativePath, `
    Select-SuperZipUniquePath, `
    Get-SuperZipChangedPath, `
    Test-SuperZipAnyPath, `
    Test-SuperZipAllPath, `
    Get-SuperZipVerificationScope, `
    Get-SuperZipVerificationPlan, `
    Invoke-SuperZipVerificationCommand
