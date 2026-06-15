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
    (Join-Path $repo "build"),
    (Join-Path $repo "out"),
    (Join-Path $repo ".vs")
)

$files = Get-ChildItem -Path $repo -Recurse -File -Force | Where-Object {
    $path = $_.FullName
    -not ($excludedRoots | Where-Object { $path.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) })
}

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
    $path = $_.FullName
    -not ($excludedRoots | Where-Object { $path.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) })
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
}

Test-WorkflowSecurityPolicy

# Purpose: Verify release MSI defaults remain aligned with the product installer contract.
# Inputs: Reads CMake, build script, and release workflow text from the repository.
# Outputs: Throws when the release MSI can drift away from the Program Files per-machine path.
function Test-InstallerScopePolicy {
    $cmakeLists = Get-Content -LiteralPath (Join-Path $repo "CMakeLists.txt") -Raw
    if ($cmakeLists -notmatch 'set\(SUPERZIP_MSI_INSTALL_SCOPE\s+"perMachine"') {
        throw "CMakeLists.txt must default SUPERZIP_MSI_INSTALL_SCOPE to perMachine for product MSI releases."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_ROOT_FOLDER_ID\s+"ProgramFiles<64>Folder"\)') {
        throw "CMakeLists.txt must set CPACK_WIX_ROOT_FOLDER_ID to ProgramFiles<64>Folder for release MSIs."
    }
    if ($cmakeLists -match 'CPACK_CREATE_DESKTOP_LINKS') {
        throw "Desktop shortcuts must be an optional MSI feature, not unconditional CPACK_CREATE_DESKTOP_LINKS."
    }
    if ($cmakeLists -notmatch 'set\(CPACK_WIX_UI_REF\s+"WixUI_FeatureTree"\)') {
        throw "CMakeLists.txt must use the WiX feature tree so Create Desktop shortcut is user-selectable."
    }
    if ($cmakeLists -notmatch 'superzip_desktop_shortcut\.wxs' -or $cmakeLists -notmatch 'superzip_wix_patch\.xml') {
        throw "CMakeLists.txt must include the optional desktop shortcut WiX source and patch files."
    }

    $desktopShortcut = Get-Content -LiteralPath (Join-Path $repo "cmake\superzip_desktop_shortcut.wxs") -Raw
    if ($desktopShortcut -notmatch 'CM_SHORTCUT_DESKTOP_OPTIONAL' -or $desktopShortcut -notmatch 'DesktopFolder') {
        throw "Optional desktop shortcut WiX source must define the MSI-owned DesktopFolder shortcut component."
    }
    $desktopPatch = Get-Content -LiteralPath (Join-Path $repo "cmake\superzip_wix_patch.xml") -Raw
    if ($desktopPatch -notmatch 'Create Desktop shortcut' -or $desktopPatch -notmatch 'ComponentRef Id="CM_SHORTCUT_DESKTOP_OPTIONAL"') {
        throw "WiX patch must expose the Create Desktop shortcut feature and reference its component."
    }

    $buildScript = Get-Content -LiteralPath (Join-Path $repo "tools\build.ps1") -Raw
    if ($buildScript -notmatch '\[string\]\$MsiInstallScope\s*=\s*"perMachine"') {
        throw "tools/build.ps1 must default -MsiInstallScope to perMachine. Use explicit perUser only for local non-admin tests."
    }

    $releaseAction = Get-Content -LiteralPath (Join-Path $repo ".github\actions\windows-release\action.yml") -Raw
    if ($releaseAction -notmatch 'MsiInstallScope\s*=\s*"perMachine"') {
        throw "Release workflow must pass MsiInstallScope=perMachine for published MSI artifacts."
    }
    if ($releaseAction -notmatch '\$expectedCli\s*=\s*Join-Path\s+\$env:ProgramFiles\s+"SuperZip\\bin\\superzip_cli\.exe"') {
        throw "Release workflow must validate the installed CLI under Program Files."
    }
}

Test-InstallerScopePolicy

Write-Host "Security scan passed."
