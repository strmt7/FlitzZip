$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$secretPatterns = @(
    "ghp_[A-Za-z0-9_]{30,}",
    "github_pat_[A-Za-z0-9_]+",
    "sk-[A-Za-z0-9]{20,}",
    "AKIA[0-9A-Z]{16}"
)

$excludedRoots = @(
    (Join-Path $repo ".git"),
    (Join-Path $repo ".psmodules"),
    (Join-Path $repo ".ruff_cache"),
    (Join-Path $repo ".venv-lint"),
    (Join-Path $repo "build"),
    (Join-Path $repo "out"),
    (Join-Path $repo "skills"),
    (Join-Path $repo ".vs")
)
$excludedFragments = @(
    "\__pycache__\"
)

# Purpose: Decide whether a file is generated local state outside source/security scanning scope.
# Inputs: `Path` is a candidate absolute file path under the repository.
# Outputs: Returns true for build products, local tool caches, Python bytecode caches, and ignored generated folders.
function Test-ExcludedScanPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    foreach ($root in $excludedRoots) {
        $normalizedRoot = [IO.Path]::GetFullPath($root).TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar)
        if ($full.Equals($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
            $full.StartsWith($normalizedRoot + [IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -or
            $full.StartsWith($normalizedRoot + [IO.Path]::AltDirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    foreach ($fragment in $excludedFragments) {
        if ($full.IndexOf($fragment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

# Purpose: Verify release replacement remains guarded by an explicit version-specific acknowledgement.
# Inputs: Reads the release workflow and composite release action from the repository.
# Outputs: Throws when replacement can delete an existing release/tag without the acknowledgement gate.
function Assert-ReleaseReplacementSafeguard {
    $releaseWorkflow = Join-Path $repo ".github\workflows\release.yml"
    $releaseAction = Join-Path $repo ".github\actions\windows-release\action.yml"
    if (-not (Test-Path -LiteralPath $releaseWorkflow) -or -not (Test-Path -LiteralPath $releaseAction)) {
        throw "Release workflow and windows-release action must both exist for replacement safeguard validation."
    }

    $workflowText = Get-Content -LiteralPath $releaseWorkflow -Raw
    $actionText = Get-Content -LiteralPath $releaseAction -Raw
    foreach ($requiredSnippet in @(
            "replacement_acknowledgement:",
            'replacement_acknowledgement: ${{ inputs.replacement_acknowledgement }}')) {
        if ($workflowText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Release workflow is missing the replacement acknowledgement safeguard: $requiredSnippet"
        }
    }
    foreach ($requiredSnippet in @(
            "replacement_acknowledgement:",
            "REPLACEMENT_ACKNOWLEDGEMENT",
            "replace_existing=true requires replacement_acknowledgement exactly",
            "REPLACE_RELEASE_TAG=`$releaseTag",
            "Replacement tag tracking mismatch",
            'MsiProductIdentity = "github-run-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT-$env:GITHUB_SHA"',
            "Replacement is exceptional")) {
        if ($actionText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Windows release action is missing the replacement acknowledgement safeguard: $requiredSnippet"
        }
    }
    if ($workflowText -notmatch [regex]::Escape("MSI replacements get a fresh ProductCode from the release run identity")) {
        throw "Release workflow input text must document same-version MSI replacement identity."
    }
}

# Purpose: Verify release notes do not duplicate the GitHub release title as a Markdown H1.
# Inputs: Reads the composite release action that generates `out\release-notes.md`.
# Outputs: Throws when the generated notes would repeat the release title inside the body.
function Assert-ReleaseNotesDoNotDuplicateTitle {
    $releaseAction = Join-Path $repo ".github\actions\windows-release\action.yml"
    if (-not (Test-Path -LiteralPath $releaseAction)) {
        throw "Windows release action must exist for release-note title validation."
    }
    $actionText = Get-Content -LiteralPath $releaseAction -Raw
    if ($actionText -match '#\s*SuperZip\s+\$env:RELEASE_TAG') {
        throw "Release notes must not include a duplicate '# SuperZip `$env:RELEASE_TAG' heading; GitHub already renders the release title."
    }
}

# Purpose: Verify release instructions stay version-agnostic except for product metadata.
# Inputs: Reads repository release instructions and agent policy text.
# Outputs: Throws when manual-release examples hardcode a SemVer value.
function Assert-ReleaseDocUseVersionPlaceholder {
    $releaseDocs = Join-Path $repo "docs\release.md"
    $agentPolicy = Join-Path $repo "AGENTS.md"
    if (-not (Test-Path -LiteralPath $releaseDocs) -or -not (Test-Path -LiteralPath $agentPolicy)) {
        throw "Release documentation and AGENTS.md must exist for release-version placeholder validation."
    }

    $releaseText = Get-Content -LiteralPath $releaseDocs -Raw
    $agentText = Get-Content -LiteralPath $agentPolicy -Raw
    if ($releaseText -match 'release_version=\d+\.\d+\.\d+') {
        throw "docs/release.md must use <new-semver> or <existing-semver> placeholders, not hardcoded release_version examples."
    }
    if ($releaseText -match 'replacement_acknowledgement="replace \d+\.\d+\.\d+"') {
        throw "docs/release.md must use a placeholder replacement acknowledgement, not a hardcoded version."
    }
    if ($agentText -match 'replace \d+\.\d+\.\d+') {
        throw "AGENTS.md must not hardcode release-replacement version examples."
    }
}

# Purpose: Verify development transfer diagnostics stay RAM-only and GPU-proof oriented.
# Inputs: Reads the development transfer diagnostic tool and documentation.
# Outputs: Throws when the diagnostic stops enforcing zero disk writes or required HIP telemetry.
function Assert-DevelopmentTransferDiagnostic {
    $tool = Join-Path $repo "tools\transfer_diagnostics.ps1"
    $doc = Join-Path $repo "docs\development-transfer-diagnostics.md"
    if (-not (Test-Path -LiteralPath $tool) -or -not (Test-Path -LiteralPath $doc)) {
        throw "Development transfer diagnostics tool and documentation must both exist."
    }
    $toolText = Get-Content -LiteralPath $tool -Raw
    foreach ($requiredSnippet in @(
            "memory-benchmark",
            "--force-cpu",
            "--require-gpu",
            "memory_only",
            "disk_write_bytes",
            "gpu_h2d_bytes",
            "gpu_d2h_bytes",
            "gpu_device_allocation_bytes",
            "gpu_kernel_launches")) {
        if ($toolText -notmatch [regex]::Escape($requiredSnippet)) {
            throw "Development transfer diagnostics must keep RAM-only CPU/GPU transfer proof: $requiredSnippet"
        }
    }
    $docText = Get-Content -LiteralPath $doc -Raw
    if ($docText -notmatch 'development-only' -or $docText -notmatch 'disk_write_bytes=0') {
        throw "Development transfer diagnostics documentation must state the development-only and zero-disk-write contract."
    }
}

# Purpose: Enumerate source-controlled scan candidates without descending excluded generated roots.
# Inputs: None; reads the repository root and applies `Test-ExcludedScanPath` before recursion.
# Outputs: Returns file objects that should be scanned for secret-like content.
function Get-SecurityScanFile {
    $rootItems = Get-ChildItem -LiteralPath $repo -Force -ErrorAction Stop
    foreach ($item in $rootItems) {
        if (Test-ExcludedScanPath -Path $item.FullName) {
            continue
        }
        if ($item.PSIsContainer) {
            Get-ChildItem -LiteralPath $item.FullName -Recurse -File -Force -ErrorAction Stop | Where-Object {
                -not (Test-ExcludedScanPath -Path $_.FullName)
            }
        } else {
            $item
        }
    }
}

$files = Get-SecurityScanFile

foreach ($file in $files) {
    $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) { continue }
    foreach ($pattern in $secretPatterns) {
        if ($text -match $pattern) {
            throw "Potential secret pattern found in $($file.FullName)"
        }
    }
}

$forbidden = Get-ChildItem -Path $repo -Recurse -File -Include *.pdb,*.ilk,*.obj,*.exe,*.dll -Force | Where-Object {
    -not (Test-ExcludedScanPath -Path $_.FullName)
}
if ($forbidden) {
    throw "Build artifacts found outside build directory: $($forbidden[0].FullName)"
}

# Purpose: Verify workflow policy rules that prevent deployments, suppressed failures, and source-scan blind spots.
# Inputs: Reads tracked GitHub workflow/configuration files from `.github`.
# Outputs: Throws when workflow policy is violated; otherwise returns normally.
function Test-WorkflowSecurityPolicy {
    $workflowRoots = @(
        (Join-Path $repo ".github\workflows"),
        (Join-Path $repo ".github\actions")
    ) | Where-Object { Test-Path -LiteralPath $_ }

    $workflowFiles = foreach ($root in $workflowRoots) {
        Get-ChildItem -Path $root -Recurse -File -Include *.yml,*.yaml
    }

    foreach ($file in $workflowFiles) {
        $lines = Get-Content -LiteralPath $file.FullName
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            $line = $lines[$i]
            if ($line -match '^\s*environment\s*:') {
                throw "Workflow environment blocks are forbidden because they can create GitHub deployment records: $($file.FullName):$($i + 1)"
            }
            if ($line -match '^\s*deployment\s*:') {
                throw "Workflow deployment keys are forbidden in this repository: $($file.FullName):$($i + 1)"
            }
            if ($line -match 'continue-on-error\s*:\s*true') {
                throw "Workflow soft-fail behavior is forbidden in $($file.FullName):$($i + 1)"
            }
            if ($line -match '(--exclude|skip-dirs|ignore-globs|paths-ignore).*(src|tests|tools|third_party|\.github)') {
                throw "Workflow scanner exclusion covers source-controlled code in $($file.FullName):$($i + 1)"
            }
            if ($line -match 'Install-PackageProvider\s+-Name\s+NuGet') {
                throw "Workflow must not rely on Install-PackageProvider NuGet bootstrap; use hash-checked package installation: $($file.FullName):$($i + 1)"
            }
        }
        Assert-NoGithubContextInRunBlock -Path $file.FullName -Lines $lines
    }

    $securityWorkflow = Join-Path $repo ".github\workflows\security-code-scanning.yml"
    if (Test-Path -LiteralPath $securityWorkflow) {
        $securityWorkflowText = Get-Content -LiteralPath $securityWorkflow -Raw
        if ($securityWorkflowText -match 'languages\s*:\s*c-cpp' -and $securityWorkflowText -match 'build-mode\s*:\s*none') {
            throw "CodeQL C++ must not use build-mode none; manual Windows build tracing is required to avoid parser-artifact alerts."
        }
        foreach ($requiredSnippet in @(
                'runs-on: windows-2022',
                'build-mode: manual',
                'tools/build.ps1 -Configuration Release -CpuOnlyValidation')) {
            if ($securityWorkflowText -notmatch [regex]::Escape($requiredSnippet)) {
                throw "CodeQL C++ workflow is missing required manual Windows build setting: $requiredSnippet"
            }
        }
    }

    $codeqlConfig = Join-Path $repo ".github\codeql\codeql-config.yml"
    if (Test-Path -LiteralPath $codeqlConfig) {
        $text = Get-Content -LiteralPath $codeqlConfig -Raw
        if ($text -match 'paths-ignore\s*:') {
            throw "CodeQL paths-ignore is forbidden because it can hide source findings: $codeqlConfig"
        }
    }

    $semgrepIgnore = Join-Path $repo ".semgrepignore"
    if (Test-Path -LiteralPath $semgrepIgnore) {
        $activePatterns = Get-Content -LiteralPath $semgrepIgnore | Where-Object {
            $trimmed = $_.Trim()
            $trimmed -and -not $trimmed.StartsWith("#")
        }
        if ($activePatterns) {
            throw ".semgrepignore must not contain active ignore patterns: $semgrepIgnore"
        }
    }

    Assert-ReleaseReplacementSafeguard
    Assert-ReleaseNotesDoNotDuplicateTitle
    Assert-ReleaseDocUseVersionPlaceholder
}

# Purpose: Return the count of leading spaces before the first non-space character.
# Inputs: `Line` is one workflow YAML line.
# Outputs: Returns an indentation count used for block-scalar boundary checks.
function Get-LeadingSpaceCount {
    param([AllowEmptyString()][string]$Line)

    $match = [regex]::Match($Line, '^\s*')
    return $match.Value.Length
}

# Purpose: Test whether one workflow YAML line references untrusted GitHub context.
# Inputs: `Line` is a single workflow YAML line.
# Outputs: Returns true when the line contains a GitHub context expression.
function Test-GithubContextInterpolation {
    param([AllowEmptyString()][string]$Line)

    $expressionPrefix = [regex]::Escape('$') + [regex]::Escape(([string][char]123) + ([string][char]123))
    return $Line -match ($expressionPrefix + '\s*github\.')
}

# Purpose: Reject direct GitHub context interpolation inside workflow `run` scripts.
# Inputs: `Path` identifies the workflow file and `Lines` contains its text.
# Outputs: Throws when a run block can be vulnerable to script injection.
function Assert-NoGithubContextInRunBlock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Lines
    )

    $inRunBlock = $false
    $runIndent = -1
    for ($i = 0; $i -lt $Lines.Count; ++$i) {
        $line = $Lines[$i]
        $trimmed = $line.Trim()
        if ($inRunBlock -and $trimmed.Length -gt 0 -and (Get-LeadingSpaceCount -Line $line) -le $runIndent) {
            $inRunBlock = $false
        }
        if ($inRunBlock -and (Test-GithubContextInterpolation -Line $line)) {
            throw "Workflow run block interpolates github context directly. Use env indirection instead: $($Path):$($i + 1)"
        }
        if ($line -match '^(\s*)run\s*:\s*(\||>|$)') {
            $inRunBlock = $true
            $runIndent = $Matches[1].Length
        } elseif ($line -match '^\s*run\s*:' -and (Test-GithubContextInterpolation -Line $line)) {
            throw "Workflow run command interpolates github context directly. Use env indirection instead: $($Path):$($i + 1)"
        }
    }
}

Test-WorkflowSecurityPolicy
Assert-ReleaseDocUseVersionPlaceholder
Assert-DevelopmentTransferDiagnostic

# Purpose: Verify README badges avoid known flaky GitHub API-backed shields.
# Inputs: Reads `README.md`.
# Outputs: Throws when the license badge depends on the shields GitHub license endpoint.
function Test-ReadmeBadgePolicy {
    $readme = Get-Content -LiteralPath (Join-Path $repo "README.md") -Raw
    if ($readme -match 'img\.shields\.io/github/license/strmt7/SuperZip') {
        throw "README license badge must use the static AGPL-3.0 badge to avoid shields GitHub API token-pool failures."
    }
    if ($readme -notmatch 'resources/brand/superzip-logo\.svg') {
        throw "README must render the product logo from resources/brand/superzip-logo.svg."
    }
}

Test-ReadmeBadgePolicy

# Purpose: Keep external product comparison names out of repo docs and source after one-time audit use.
# Inputs: Scans source-controlled text outside generated/build folders.
# Outputs: Throws when an intentionally omitted comparison name is reintroduced.
function Test-ExternalComparisonNamePolicy {
    $forbiddenNames = @(
        ("ban" + "dizip"),
        ("ban" + "disoft")
    )
    $textExtensions = @(
        ".md", ".txt", ".ps1", ".psm1", ".yml", ".yaml", ".json", ".cmake",
        ".cpp", ".hpp", ".h", ".c", ".rc", ".wxs", ".xml", ".svg"
    )
    $policyFiles = Get-ChildItem -Path $repo -Recurse -File -Force | Where-Object {
        $path = $_.FullName
        ($textExtensions -contains $_.Extension.ToLowerInvariant()) -and
        -not ($excludedRoots | Where-Object { $path.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) })
    }
    foreach ($file in $policyFiles) {
        $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
        if ($null -eq $text) { continue }
        foreach ($name in $forbiddenNames) {
            if ($text.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "Forbidden external comparison name found in $($file.FullName). Keep only product-agnostic audit logic in this repository."
            }
        }
    }
}

Test-ExternalComparisonNamePolicy

# Purpose: Verify SuperZip-owned UI cannot expose copyable text surfaces or window-wide drop affordances.
# Inputs: Reads native GUI source files.
# Outputs: Throws when app-owned UI uses clipboard APIs, editable text controls, or full-window drop registration.
function Test-GuiOwnedSurfacePolicy {
    $appSources = Get-ChildItem -LiteralPath (Join-Path $repo "src\app") -File -Recurse -Include *.cpp,*.hpp,*.h,*.rc
    $sourceText = ($appSources | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    if ($sourceText.Contains('DragAcceptFiles(hwnd_, TRUE)') -or $sourceText.Contains('WS_EX_ACCEPTFILES')) {
        throw "SuperZip-owned drag/drop must be accepted only by the Queue OLE drop target, not the full HWND."
    }
    if (-not $sourceText.Contains('is_copy_accelerator')) {
        throw "SuperZip-owned UI must consume text-copy accelerators before page-specific key handling."
    }
    if ($sourceText -match 'OpenClipboard|SetClipboardData|GetClipboardData|CreateWindowExW[^\r\n]*(EDIT|RICHEDIT)|CreateWindowW[^\r\n]*(EDIT|RICHEDIT)') {
        throw "SuperZip-owned UI must not expose selectable text controls or clipboard APIs."
    }
}

Test-GuiOwnedSurfacePolicy

& (Join-Path $repo "tools\verify_brand_assets.ps1")

& (Join-Path $repo "tools\verify_license_notices.ps1")

& (Join-Path $repo "tools\refactor_audit.ps1") -ChangedOnly -CheckContracts -MaxFunctionLines 120 -MaxComplexityMarkers 35 -FailOnFindings

# Purpose: Verify CMake release MSI identity and UI defaults.
# Inputs: CMakeLists is the repository CMake source text.
# Outputs: Throws when CMake MSI metadata can drift from the product contract.
function Test-InstallerCMakePolicy {
    param([Parameter(Mandatory = $true)][string]$CMakeLists)

    if ($cmakeLists -notmatch 'set\(SUPERZIP_MSI_INSTALL_SCOPE\s+"perMachine"') {
        throw "CMakeLists.txt must default SUPERZIP_MSI_INSTALL_SCOPE to perMachine for product MSI releases."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_ROOT_FOLDER_ID\s+"ProgramFiles<64>Folder"\)') {
        throw "CMakeLists.txt must set CPACK_WIX_ROOT_FOLDER_ID to ProgramFiles<64>Folder for release MSIs."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_PACKAGE_VENDOR\s+"Efstratios Mitridis"\)') {
        throw "CMakeLists.txt must set the release MSI publisher to Efstratios Mitridis."
    }
    if ($cmakeLists -notmatch 'set\(SUPERZIP_WIX_UPGRADE_GUID\s+"8E0D80F6-859D-4FE9-B082-F0D8048A8B57"\)') {
        throw "CMakeLists.txt must keep SuperZip's stable MSI UpgradeCode in one explicit variable."
    }
    if ($cmakeLists -notmatch 'SUPERZIP_WIX_PRODUCT_CODE_SEED[\s\S]*SUPERZIP_PACKAGE_NUMERIC_VERSION[\s\S]*SUPERZIP_MSI_INSTALL_SCOPE[\s\S]*SUPERZIP_MSI_PRODUCT_IDENTITY') {
        throw "CMakeLists.txt must seed the MSI ProductCode from package numeric version, install scope, and package identity."
    }
    if ($cmakeLists -notmatch 'string\(SHA256\s+SUPERZIP_WIX_PRODUCT_CODE_HASH[\s\S]*SUPERZIP_WIX_PRODUCT_CODE_SEED') {
        throw "CMakeLists.txt must derive the MSI ProductCode from a SHA-256 identity seed."
    }
    if ($cmakeLists -notmatch 'string\(SUBSTRING\s+"\$\{SUPERZIP_WIX_PRODUCT_CODE_HASH\}"[\s\S]*string\(TOUPPER\s+"\$\{SUPERZIP_WIX_PRODUCT_GUID\}"') {
        throw "CMakeLists.txt must format the SHA-256-derived MSI ProductCode as an uppercase GUID."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_PRODUCT_GUID\s+"\$\{SUPERZIP_WIX_PRODUCT_GUID\}"\)') {
        throw "CMakeLists.txt must pass the deterministic MSI ProductCode to CPack WiX."
    }
    if ($cmakeLists -match 'CPACK_CREATE_DESKTOP_LINKS') {
        throw "Desktop shortcuts must be an optional MSI feature, not unconditional CPACK_CREATE_DESKTOP_LINKS."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_UI_REF\s+"WixUI_FeatureTree"\)') {
        throw "CMakeLists.txt must use the WiX feature tree so Create Desktop shortcut is user-selectable."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_PRODUCT_ICON\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/resources/app/superzip\.ico"\)') {
        throw "CMakeLists.txt must set CPACK_WIX_PRODUCT_ICON to the canonical SuperZip icon for Add/Remove Programs."
    }
    if ($cmakeLists -notmatch 'superzip_desktop_shortcut\.wxs' -or $cmakeLists -notmatch 'superzip_wix_patch\.xml') {
        throw "CMakeLists.txt must include the optional shortcut WiX source and patch files."
    }
}

# Purpose: Verify optional MSI shortcut WiX fragments remain user-selectable.
# Inputs: DesktopShortcut and DesktopPatch are the WiX source and patch text.
# Outputs: Throws when shortcut options stop being explicit MSI features.
function Test-InstallerShortcutPolicy {
    param(
        [Parameter(Mandatory = $true)][string]$DesktopShortcut,
        [Parameter(Mandatory = $true)][string]$DesktopPatch
    )

    if ($desktopShortcut -notmatch 'CM_SHORTCUT_DESKTOP_OPTIONAL' -or $desktopShortcut -notmatch 'DesktopFolder') {
        throw "Optional desktop shortcut WiX source must define the MSI-owned DesktopFolder shortcut component."
    }
    if ($desktopShortcut -notmatch 'CM_SHORTCUT_START_MENU_OPTIONAL' -or $desktopShortcut -notmatch 'ProgramMenuFolder' -or $desktopShortcut -notmatch 'CM_REMOVE_SUPERZIP_PROGRAM_MENU_FOLDER') {
        throw "Optional Start Menu WiX source must define the MSI-owned ProgramMenuFolder shortcut component and uninstall cleanup."
    }
    if ($desktopShortcut -notmatch 'Software\\Efstratios Mitridis\\SuperZip') {
        throw "Optional shortcut registry markers must use the Efstratios Mitridis publisher key."
    }
    if ($desktopPatch -notmatch 'Create Desktop shortcut' -or $desktopPatch -notmatch 'ComponentRef Id="CM_SHORTCUT_DESKTOP_OPTIONAL"') {
        throw "WiX patch must expose the Create Desktop shortcut feature and reference its component."
    }
    if ($desktopPatch -notmatch 'Create Start Menu entry' -or $desktopPatch -notmatch 'ComponentRef Id="CM_SHORTCUT_START_MENU_OPTIONAL"') {
        throw "WiX patch must expose the Create Start Menu entry feature and reference its component."
    }
}

# Purpose: Verify build/package scripts preserve MSI identity validation.
# Inputs: Script text from build, package, MSI identity, and verifier routing.
# Outputs: Throws when local or release packaging can miss MSI identity checks.
function Test-InstallerPackagingScriptPolicy {
    param(
        [Parameter(Mandatory = $true)][string]$BuildScript,
        [Parameter(Mandatory = $true)][string]$PackageScript,
        [Parameter(Mandatory = $true)][string]$MsiIdentityTool,
        [Parameter(Mandatory = $true)][string]$VerificationSelector
    )

    if ($buildScript -notmatch '\[string\]\$MsiInstallScope\s*=\s*"perMachine"') {
        throw "tools/build.ps1 must default -MsiInstallScope to perMachine. Use explicit perUser only for local non-admin tests."
    }
    if ($buildScript -notmatch '\[string\]\$MsiProductIdentity\s*=' -or $buildScript -notmatch 'SUPERZIP_MSI_PRODUCT_IDENTITY=\$MsiProductIdentity') {
        throw "tools/build.ps1 must pass an explicit MSI product identity into CMake ProductCode derivation."
    }

    if ($packageScript -notmatch 'Read-MsiProperty\s+-MsiPath\s+\$msi\s+-Name\s+"ProductCode"' -or $packageScript -notmatch 'CPACK_WIX_PRODUCT_GUID') {
        throw "tools/package.ps1 must verify the generated MSI ProductCode against CPack's deterministic ProductCode."
    }
    if ($packageScript -notmatch 'Read-MsiProperty\s+-MsiPath\s+\$msi\s+-Name\s+"UpgradeCode"' -or $packageScript -notmatch 'CPACK_WIX_UPGRADE_GUID') {
        throw "tools/package.ps1 must verify the generated MSI UpgradeCode before publishing."
    }

    if ($msiIdentityTool -notmatch 'Same-version replacement identity did not change the MSI ProductCode' -or $msiIdentityTool -notmatch 'Patch-version update did not change the MSI ProductCode') {
        throw "tools/test_msi_identity.ps1 must verify same-build stability, same-version replacement identity changes, and patch-version ProductCode changes."
    }
    if ($verificationSelector -notmatch 'msi-identity-smoke' -or $verificationSelector -notmatch 'tools/test_msi_identity\.ps1') {
        throw "Packaging verification must route through tools/test_msi_identity.ps1."
    }
}

# Purpose: Verify release workflow MSI validation uses per-machine bounded paths.
# Inputs: ReleaseAction is the composite release action YAML text.
# Outputs: Throws when hosted release validation can publish unchecked MSI output.
function Test-InstallerReleaseActionPolicy {
    param([Parameter(Mandatory = $true)][string]$ReleaseAction)

    if ($releaseAction -notmatch 'MsiInstallScope\s*=\s*"perMachine"') {
        throw "Release workflow must pass MsiInstallScope=perMachine for published MSI artifacts."
    }
    if ($releaseAction -notmatch '\$expectedCli\s*=\s*Join-Path\s+\$env:ProgramFiles\s+"SuperZip\\bin\\superzip_cli\.exe"') {
        throw "Release workflow must validate the installed CLI under Program Files."
    }
    if ($releaseAction -match 'Start-Process\s+-FilePath\s+\$installer[^\r\n]*-Wait') {
        throw "HIP SDK installer launches in the release workflow must use a bounded WaitForExit timeout, not unbounded Start-Process -Wait."
    }
    if ($releaseAction -match 'Start-Process\s+(?:-FilePath\s+)?msiexec\.exe[^\r\n]*-Wait') {
        throw "Release MSI smoke tests must use a bounded WaitForExit timeout, not unbounded Start-Process -Wait."
    }
    if ($releaseAction -notmatch 'AMD HIP SDK installer timed out after \$hipInstallTimeoutSeconds seconds') {
        throw "Release workflow must fail stale HIP SDK installer waits with an explicit bounded-timeout message."
    }
    if ($releaseAction -notmatch '\[int\]\$TimeoutSeconds\s*=\s*300') {
        throw "Release MSI install/repair/uninstall smoke tests must default to a 300-second stale-wait timeout."
    }
    if ($releaseAction -notmatch 'installer or elevation prompt was left unanswered') {
        throw "Release installer timeout messages must explain that unanswered elevation prompts are a possible cause."
    }
    if ($releaseAction -notmatch 'MSI repair' -or $releaseAction -notmatch 'superzip-msi-repair-dependency-check\.txt') {
        throw "Release MSI smoke tests must repair the just-installed MSI and run dependency-check after repair."
    }
}

# Purpose: Verify release MSI defaults remain aligned with the installer contract.
# Inputs: Reads installer-related CMake, WiX, script, and workflow text.
# Outputs: Throws when the release MSI can drift from the per-machine contract.
function Test-InstallerScopePolicy {
    $cmakeLists = Get-Content -LiteralPath (Join-Path $repo "CMakeLists.txt") -Raw
    Test-InstallerCMakePolicy -CMakeLists $cmakeLists

    $desktopShortcut = Get-Content -LiteralPath (Join-Path $repo "cmake\superzip_desktop_shortcut.wxs") -Raw
    $desktopPatch = Get-Content -LiteralPath (Join-Path $repo "cmake\superzip_wix_patch.xml") -Raw
    Test-InstallerShortcutPolicy -DesktopShortcut $desktopShortcut -DesktopPatch $desktopPatch

    $buildScript = Get-Content -LiteralPath (Join-Path $repo "tools\build.ps1") -Raw
    $packageScript = Get-Content -LiteralPath (Join-Path $repo "tools\package.ps1") -Raw
    $msiIdentityTool = Get-Content -LiteralPath (Join-Path $repo "tools\test_msi_identity.ps1") -Raw
    $verificationSelector = Get-Content -LiteralPath (Join-Path $repo "tools\superzip_verification.psm1") -Raw
    Test-InstallerPackagingScriptPolicy `
        -BuildScript $buildScript `
        -PackageScript $packageScript `
        -MsiIdentityTool $msiIdentityTool `
        -VerificationSelector $verificationSelector

    $releaseAction = Get-Content -LiteralPath (Join-Path $repo ".github\actions\windows-release\action.yml") -Raw
    Test-InstallerReleaseActionPolicy -ReleaseAction $releaseAction
}

Test-InstallerScopePolicy

# Normalize handled native-command exit codes so pwsh returns the logical scan result.
$global:LASTEXITCODE = 0
Write-Output "Security scan passed."
