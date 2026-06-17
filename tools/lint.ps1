param(
    [ValidateSet("Changed", "All", "Off")]
    [string]$CppMode = "Changed",
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

# Purpose: Invoke a native command and fail when it returns a non-zero exit code.
# Inputs: `FilePath` is the executable, `Arguments` is argv, and `Label` describes the lint step.
# Outputs: Throws on lint failure; otherwise returns normally.
function Invoke-LintCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Label
    )

    Write-Output "lint step=$Label"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

# Purpose: Return tracked files matching one or more git pathspecs.
# Inputs: `Pathspec` is passed directly to `git ls-files`.
# Outputs: Returns repository-relative file paths using slash separators.
function Get-TrackedLintFile {
    param([Parameter(Mandatory = $true)][string[]]$Pathspec)

    Push-Location $repo
    try {
        return @(& git ls-files -- @Pathspec)
    } finally {
        Pop-Location
    }
}

# Purpose: Return changed files for language lint targeting.
# Inputs: `BaseRef` and `HeadRef` optionally define a git diff range; `IncludeUntracked` adds new local files.
# Outputs: Returns repository-relative file paths using slash separators.
function Get-ChangedLintFile {
    param(
        [string]$BaseRef,
        [string]$HeadRef,
        [switch]$IncludeUntracked
    )

    Push-Location $repo
    try {
        $paths = @()
        if (-not [string]::IsNullOrWhiteSpace($BaseRef)) {
            if ($BaseRef -match '^0{40}$') {
                $paths += & git ls-files
            } else {
                $gitArgs = @("diff", "--name-only", "--diff-filter=ACMRTUX", $BaseRef)
                if (-not [string]::IsNullOrWhiteSpace($HeadRef)) {
                    $gitArgs += $HeadRef
                }
                $paths += & git @gitArgs
            }
        } else {
            $paths += & git diff --name-only --diff-filter=ACMRTUX
            $paths += & git diff --cached --name-only --diff-filter=ACMRTUX
            if ($IncludeUntracked.IsPresent) {
                $paths += & git ls-files --others --exclude-standard
            }
        }
        return @($paths | Sort-Object -Unique)
    } finally {
        Pop-Location
    }
}

# Purpose: Select files whose repository-relative path matches any regular expression.
# Inputs: `Path` is a path list and `Pattern` is one or more anchored regular expressions.
# Outputs: Returns unique matching paths in stable order.
function Select-LintFile {
    param(
        [string[]]$Path,
        [string[]]$Pattern
    )

    $selected = foreach ($pathItem in @($Path)) {
        foreach ($patternItem in @($Pattern)) {
            if ($pathItem -match $patternItem) {
                $pathItem
                break
            }
        }
    }
    return @($selected | Sort-Object -Unique)
}

# Purpose: Detect whether any changed path requires a whole-language lint pass.
# Inputs: `Path` is a changed path list and `Pattern` names linter config files.
# Outputs: Returns true when a config change should expand the lint scope.
function Test-LintConfigChange {
    param(
        [string[]]$Path,
        [string[]]$Pattern
    )

    return (Select-LintFile -Path $Path -Pattern $Pattern).Count -gt 0
}

# Purpose: Build a lint target set from changed files, or all tracked files when requested.
# Inputs: Changed paths, all-file pathspecs, file patterns, config patterns, and `ForceAll`.
# Outputs: Returns repository-relative lint target paths.
function Get-LintTargetFile {
    param(
        [string[]]$ChangedPath,
        [string[]]$AllPathspec,
        [string[]]$FilePattern,
        [string[]]$ConfigPattern = @(),
        [switch]$ForceAll
    )

    if ($ForceAll.IsPresent -or (Test-LintConfigChange -Path $ChangedPath -Pattern $ConfigPattern)) {
        return Get-TrackedLintFile -Pathspec $AllPathspec
    }
    return Select-LintFile -Path $ChangedPath -Pattern $FilePattern
}

# Purpose: Select C/C++ files owned by SuperZip rather than vendored upstream code.
# Inputs: `Path` contains repository-relative paths.
# Outputs: Returns source, test, and fuzz C/C++ files suitable for clang-format checks.
function Select-SuperZipCppLintFile {
    param([string[]]$Path)

    return @($Path | Where-Object {
            $_ -match '^(src|tests|fuzz)/.*\.(c|cc|cpp|h|hpp)$'
        } | Sort-Object -Unique)
}

# Purpose: Run PSScriptAnalyzer and fail on any finding.
# Inputs: `Path` contains PowerShell script/module files to inspect.
# Outputs: Throws when PSScriptAnalyzer returns findings.
function Invoke-PowerShellLint {
    param([Parameter(Mandatory = $true)][string[]]$Path)

    Write-Output "lint step=powershell-psscriptanalyzer"
    Import-Module PSScriptAnalyzer -ErrorAction Stop
    $findings = foreach ($scriptPath in @($Path)) {
        Invoke-ScriptAnalyzer -Path $scriptPath -Severity Error, Warning
    }
    if ($findings.Count -gt 0) {
        $findings | Format-Table -AutoSize | Out-String | Write-Output
        throw "powershell-psscriptanalyzer found $($findings.Count) issue(s)."
    }
}

Push-Location $repo
try {
    $changedFiles = Get-ChangedLintFile -BaseRef $BaseRef -HeadRef $HeadRef -IncludeUntracked:$IncludeUntracked

    $pythonFiles = Get-LintTargetFile `
        -ChangedPath $changedFiles `
        -AllPathspec @("mcp/*.py", "tools/*.py") `
        -FilePattern @("^(mcp|tools)/.*\.py$") `
        -ConfigPattern @("^\.ruff\.toml$") `
        -ForceAll:($CppMode -eq "All")
    if ($pythonFiles.Count -gt 0) {
        Invoke-LintCommand -FilePath "ruff" -Arguments (@("check") + $pythonFiles) -Label "python-ruff-check"
        Invoke-LintCommand -FilePath "ruff" -Arguments (@("format", "--check") + $pythonFiles) -Label "python-ruff-format"
    } else {
        Write-Output "lint step=python-ruff skipped=no-python-files"
    }

    $powershellFiles = Get-LintTargetFile `
        -ChangedPath $changedFiles `
        -AllPathspec @("tools/*.ps1", "tools/*.psm1") `
        -FilePattern @("^tools/.*\.(ps1|psm1)$") `
        -ForceAll:($CppMode -eq "All")
    if ($powershellFiles.Count -gt 0) {
        Invoke-PowerShellLint -Path $powershellFiles
    } else {
        Write-Output "lint step=powershell-psscriptanalyzer skipped=no-powershell-files"
    }

    $yamlFiles = Get-LintTargetFile `
        -ChangedPath $changedFiles `
        -AllPathspec @(".github/*.yml", ".github/*.yaml", ".github/**/*.yml", ".github/**/*.yaml") `
        -FilePattern @("^\.github/.*\.ya?ml$") `
        -ConfigPattern @("^\.yamllint$") `
        -ForceAll:($CppMode -eq "All")
    if ($yamlFiles.Count -gt 0) {
        Invoke-LintCommand -FilePath "yamllint" -Arguments (@("-c", ".yamllint") + $yamlFiles) -Label "yaml-yamllint"
    } else {
        Write-Output "lint step=yaml-yamllint skipped=no-yaml-files"
    }

    $markdownFiles = Get-LintTargetFile `
        -ChangedPath $changedFiles `
        -AllPathspec @("README.md", "AGENTS.md", "docs/*.md", "docs/**/*.md") `
        -FilePattern @("^(README|AGENTS)\.md$", "^docs/.*\.md$") `
        -ConfigPattern @("^\.pymarkdown\.json$") `
        -ForceAll:($CppMode -eq "All")
    if ($markdownFiles.Count -gt 0) {
        Invoke-LintCommand -FilePath "pymarkdown" -Arguments (@("--config", ".pymarkdown.json", "scan") + $markdownFiles) -Label "markdown-pymarkdown"
    } else {
        Write-Output "lint step=markdown-pymarkdown skipped=no-markdown-files"
    }

    $cmakeFiles = Get-LintTargetFile `
        -ChangedPath $changedFiles `
        -AllPathspec @("CMakeLists.txt", "cmake/*.cmake", "cmake/**/*.cmake") `
        -FilePattern @("^CMakeLists\.txt$", "^cmake/.*\.cmake$") `
        -ForceAll:($CppMode -eq "All")
    if ($cmakeFiles.Count -gt 0) {
        Invoke-LintCommand -FilePath "cmake-lint" -Arguments $cmakeFiles -Label "cmake-lint"
    } else {
        Write-Output "lint step=cmake-lint skipped=no-cmake-files"
    }

    if ($CppMode -ne "Off") {
        $cppFiles = if ($CppMode -eq "All") {
            Select-SuperZipCppLintFile -Path (Get-TrackedLintFile -Pathspec @("src/*", "tests/*", "fuzz/*"))
        } else {
            Select-SuperZipCppLintFile -Path $changedFiles
        }
        if ($cppFiles.Count -gt 0) {
            Invoke-LintCommand -FilePath "clang-format" -Arguments (@("--dry-run", "--Werror") + $cppFiles) -Label "cpp-clang-format-$CppMode"
        } else {
            Write-Output "lint step=cpp-clang-format-$CppMode skipped=no-owned-cpp-files"
        }
    }
} finally {
    Pop-Location
}
