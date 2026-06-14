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
    (Join-Path $repo ".vs"),
    (Join-Path $repo "skills")
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

Write-Host "Security scan passed."
