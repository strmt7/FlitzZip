$Script:SuperZipVerificationRepoRoot = Split-Path -Parent $PSScriptRoot

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
            $args = @("diff", "--name-only", "--diff-filter=ACDMRTUX", $BaseRef)
            if (-not [string]::IsNullOrWhiteSpace($HeadRef)) {
                $args += $HeadRef
            }
            $paths += & git @args
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
function New-SuperZipVerificationCommand {
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
        '^tools/(superzip_verification\.psm1|verification_plan\.ps1|verify_changes\.ps1|verify_change_hygiene\.ps1|wait_relevant_workflows\.ps1|security_scan\.ps1|github_post_push_audit\.ps1|refactor_audit\.ps1|test\.ps1|build\.ps1|fuzz\.ps1)$',
        '^mcp/',
        '^\.agents/skills/'
    )
    $touchesCpp = Test-SuperZipAnyPath -Path $paths -Pattern @('^(src|tests|fuzz)/.*\.(cpp|hpp|c|h|rc)$', '^CMakeLists\.txt$', '^cmake/', '^third_party/(?!upstream/)')
    $touchesProductionSource = Test-SuperZipAnyPath -Path $paths -Pattern @('^src/', '^CMakeLists\.txt$', '^cmake/', '^third_party/(?!upstream/)')
    $touchesArchiveParser = Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^src/(ar|arc|arj|base64|bzip2|cab|cpio|gzip|hqx|iso|lha|lzip|lzma|macbinary|rpm|sevenzip|tar|unix_compress|uue|wim|xar|xxe|xz|zip|zstd)/',
        '^src/core/(archive|archive_format|archive_index|file_manifest|file_publish|path_safety|result|progress)\.',
        '^tests/cpp/test_.*(compat|archive|path|format).*\.cpp$',
        '^fuzz/'
    )
    $touchesSecurityBoundary = $touchesArchiveParser -or (Test-SuperZipAnyPath -Path $paths -Pattern @(
        '^src/core/(defender_scan|integrity|path_safety|file_publish)\.',
        '^tools/(security_scan|github_post_push_audit|verify_change_hygiene|wait_relevant_workflows)\.ps1$',
        '^\.github/'
    ))
    $touchesGui = Test-SuperZipAnyPath -Path $paths -Pattern @('^src/app/', '^resources/(design|app|brand)/', '^tools/(gui_smoke|generate_app_icon|generate_brand_logo_header|verify_brand_assets)\.ps1$')
    $touchesBrand = Test-SuperZipAnyPath -Path $paths -Pattern @('^resources/brand/', '^resources/app/', '^tools/(generate_app_icon|generate_brand_logo_header|verify_brand_assets)\.ps1$', '^src/app/superzip_brand_logo')
    $touchesPackaging = Test-SuperZipAnyPath -Path $paths -Pattern @('^CMakeLists\.txt$', '^cmake/', '^tools/(package|install_wix|build|version|release_metadata)\.(ps1|py)$', '^\.github/actions/windows-release/', '^\.github/workflows/release\.yml$')
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
        touchesPerformance = $touchesPerformance
        touchesMcp = $touchesMcp
        unknownPaths = @($unknown)
        fullEscalationRequired = (@($reasons).Count -gt 0)
        fullEscalationReasons = @($reasons)
    }
}

# Purpose: Build a targeted local and post-push verification plan from changed paths.
# Inputs: `ChangedPath` or git range describes the change; `SuspectGlobalBug` forces full verification.
# Outputs: Returns a plan object with scope, required commands, manual commands, workflows, and audit requirements.
function Get-SuperZipVerificationPlan {
    param(
        [string[]]$ChangedPath = @(),
        [string]$BaseRef = "",
        [string]$HeadRef = "HEAD",
        [switch]$IncludeUntracked,
        [switch]$SuspectGlobalBug
    )

    $paths = Get-SuperZipChangedPath -ChangedPath $ChangedPath -BaseRef $BaseRef -HeadRef $HeadRef -IncludeUntracked:$IncludeUntracked
    $scope = Get-SuperZipVerificationScope -ChangedPath $paths -SuspectGlobalBug:$SuspectGlobalBug
    $local = New-Object System.Collections.ArrayList
    $manual = New-Object System.Collections.ArrayList
    $seen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    $manualSeen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    $hygieneArguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/verify_change_hygiene.ps1")
    if (@($paths).Count -gt 0) {
        $hygieneArguments += "-ChangedPathBase64"
        $jsonPaths = ConvertTo-Json -InputObject @($paths) -Compress
        $hygieneArguments += [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($jsonPaths))
    }

    Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "changed-hygiene" -Stage "local" -Executable "powershell" -Arguments $hygieneArguments -Reason "always scan changed files for secrets, forbidden workflow deployment keys, generated binaries, and comparison-name policy")

    if ($scope.docsOnly -and -not $scope.fullEscalationRequired) {
        $workflows = @()
    } else {
        if ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesGui -or $scope.touchesPackaging -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "release-build" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/build.ps1", "-Configuration", "Release") -Reason "compiled product, CMake, package, GUI, or broad verification changes require a Release build")
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "unit-tests" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test.ps1", "-Configuration", "Release") -Reason "compiled product or shared verification changes require the C++ test harness")
        }
        if ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "changed-refactor-audit" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/refactor_audit.ps1", "-ChangedOnly", "-CheckContracts", "-MaxFunctionLines", "120", "-MaxComplexityMarkers", "35", "-FailOnFindings") -Reason "changed source and verification code must stay small, documented, and reviewable")
        }
        if ($scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "verification-selector-self-test" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test_verification_selector.ps1") -Reason "verification tooling or full escalation requires classifier scenario self-tests")
        }
        if ($scope.touchesSecurityBoundary -or $scope.touchesWorkflow -or $scope.touchesPackaging -or $scope.touchesVerification -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "security-scan" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/security_scan.ps1") -Reason "security boundaries, workflows, packaging, or verifier changes require repository policy checks")
        }
        if ($scope.touchesGui -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "gui-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/gui_smoke.ps1", "-Configuration", "Release") -Reason "GUI or broad changes require all tabs, buttons, toggles, dropdowns, drag/drop, and screenshots")
        }
        if ($scope.touchesBrand -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "brand-assets" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/verify_brand_assets.ps1") -Reason "brand assets changed or broad verification requires logo single-source-of-truth validation")
        }
        if ($scope.touchesArchiveParser -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "short-fuzz-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/fuzz.ps1", "-Runs", "16") -Reason "archive parser or broad changes require a bounded sanitizer/fuzzer smoke")
        }
        if ($scope.touchesPackaging -or $scope.fullEscalationRequired) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "package-smoke" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/package.ps1", "-Configuration", "Release") -Reason "installer, CPack, versioning, or broad changes require package validation")
        }
        if ($scope.touchesMcp -or $scope.touchesVerification) {
            Add-SuperZipVerificationCommand -List $local -Seen $seen -Command (New-SuperZipVerificationCommand -Id "mcp-python-compile" -Stage "local" -Executable "py" -Arguments @("-3", "-m", "py_compile", "mcp/superzip_mcp.py") -Reason "MCP Python changes require syntax validation")
        }
    }

    if ($scope.touchesPerformance -or $scope.fullEscalationRequired) {
        Add-SuperZipVerificationCommand -List $manual -Seen $manualSeen -Command (New-SuperZipVerificationCommand -Id "ram-benchmark-sweep" -Stage "local" -Executable "powershell" -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/bench.ps1", "-Configuration", "Release", "-SizeMiB", "10240", "-Profile", "Mixed", "-CompressionLevel", "5", "-Iterations", "1", "-BlockSizeKiB", "256,1024,4096,16384") -Reason "performance-sensitive changes need the RAM-only CPU/GPU benchmark sweep before making speed claims" -Requirement "manual")
    }

    $workflows = New-Object System.Collections.ArrayList
    $workflowSeen = New-Object "System.Collections.Generic.HashSet[string]" ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($pair in @(
        @("windows-ci", ($scope.touchesCpp -or $scope.touchesProductionSource -or $scope.touchesGui -or $scope.touchesPackaging -or $scope.fullEscalationRequired)),
        @("security", ($scope.touchesSecurityBoundary -or $scope.touchesWorkflow -or $scope.touchesPackaging -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("fuzzing", ($scope.touchesArchiveParser -or $scope.fullEscalationRequired)),
        @("greenbone-openvas-vulnetix", ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)),
        @("scorecard", ($scope.touchesWorkflow -or $scope.fullEscalationRequired))
    )) {
        if ($pair[1] -and $workflowSeen.Add([string]$pair[0])) {
            [void]$workflows.Add([string]$pair[0])
        }
    }

    $postPushAuditRequired = ($scope.touchesWorkflow -or $scope.touchesVerification -or $scope.fullEscalationRequired)
    $immediateWaitReasons = @()
    if ($scope.touchesWorkflow) { $immediateWaitReasons += "workflow or scanner configuration changed" }
    if ($scope.touchesVerification) { $immediateWaitReasons += "verification tooling, MCP, or agent skills changed" }
    if ($scope.fullEscalationRequired) { $immediateWaitReasons += "full verification escalation is required" }
    $recommendedWorkflowMode = if (@($workflows).Count -eq 0) {
        "none"
    } elseif ($postPushAuditRequired) {
        "final"
    } else {
        "opportunistic-during-iteration-final-before-handoff"
    }

    return [pscustomobject][ordered]@{
        scope = $scope
        requiredLocalCommands = @($local)
        manualLocalCommands = @($manual)
        postPushWorkflows = @($workflows)
        postPushAuditRequired = $postPushAuditRequired
        workflowWaitPolicy = [pscustomobject][ordered]@{
            immediateRequired = $postPushAuditRequired
            deferAllowed = (-not $postPushAuditRequired)
            recommendedMode = $recommendedWorkflowMode
            reasons = @($immediateWaitReasons)
        }
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }
}

# Purpose: Invoke one curated verification command from a plan.
# Inputs: `Command` is produced by `Get-SuperZipVerificationPlan`.
# Outputs: Throws when the command exits non-zero.
function Invoke-SuperZipVerificationCommand {
    param([Parameter(Mandatory = $true)]$Command)

    Push-Location $Script:SuperZipVerificationRepoRoot
    try {
        Write-Host "verification command=$($Command.id) reason=$($Command.reason)"
        & $Command.executable @($Command.arguments)
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
