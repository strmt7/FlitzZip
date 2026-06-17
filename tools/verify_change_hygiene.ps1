param(
    [string[]]$ChangedPath = @(),
    [string]$ChangedPathJson = "",
    [string]$ChangedPathBase64 = "",
    [string]$BaseRef = "",
    [string]$HeadRef = "HEAD",
    [switch]$IncludeUntracked,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingChangedPath = @()
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "superzip_verification.psm1") -Force

# Purpose: Return whether a file extension is normally safe to scan as text.
# Inputs: `Path` is a repository-relative or absolute path.
# Outputs: Returns true for source, docs, scripts, and workflow text files.
function Test-TextScanCandidate {
    param([Parameter(Mandatory = $true)][string]$Path)

    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    return @(
        ".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".rc", ".cmake",
        ".md", ".txt", ".ps1", ".psm1", ".py", ".json", ".yml", ".yaml",
        ".xml", ".wxs", ".svg", ".toml", ".ini", ".sh"
    ) -contains $extension -or [System.IO.Path]::GetFileName($Path) -eq "CMakeLists.txt"
}

# Purpose: Scan one changed text file for forbidden local secrets and policy-only comparison names.
# Inputs: `Path` is repository-relative and points to an existing text file.
# Outputs: Throws when a forbidden pattern is found.
function Test-ChangedFileTextPolicy {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-TextScanCandidate -Path $Path)) {
        return
    }
    $fullPath = Join-Path $repo ($Path -replace "/", "\")
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        return
    }
    $text = Get-Content -LiteralPath $fullPath -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) {
        return
    }

    $secretPatterns = @(
        "ghp_[A-Za-z0-9_]{30,}",
        "github_pat_[A-Za-z0-9_]+",
        "sk-[A-Za-z0-9]{20,}",
        "AKIA[0-9A-Z]{16}",
        "-----BEGIN (RSA|DSA|EC|OPENSSH|PGP) PRIVATE KEY-----",
        ("C:\\Users\\" + "[^\\\r\n]+")
    )
    foreach ($pattern in $secretPatterns) {
        if ($text -match $pattern) {
            throw "Potential secret or personal path pattern found in changed file: $Path"
        }
    }

    $forbiddenNames = @(("ban" + "dizip"), ("ban" + "disoft"))
    foreach ($name in $forbiddenNames) {
        if ($text.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Forbidden external comparison name found in changed file: $Path"
        }
    }
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

# Purpose: Reject fragile PowerShell Gallery bootstrap commands in workflows.
# Inputs: `Path` identifies the workflow and `Lines` contains its text.
# Outputs: Throws when a workflow relies on runner-local PackageManagement provider discovery.
function Assert-NoFragilePowerShellGalleryBootstrap {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Lines
    )

    for ($i = 0; $i -lt $Lines.Count; ++$i) {
        if ($Lines[$i] -match 'Install-PackageProvider\s+-Name\s+NuGet') {
            throw "Workflow must not rely on Install-PackageProvider NuGet bootstrap; use hash-checked package installation: $($Path):$($i + 1)"
        }
    }
}

# Purpose: Validate workflow-specific policy for changed YAML files.
# Inputs: `Path` is repository-relative and points under `.github`.
# Outputs: Throws when workflow changes could create deployments or hide failures/findings.
function Test-ChangedWorkflowPolicy {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ($Path -notmatch '^\.github/(workflows|actions)/.*\.ya?ml$') {
        return
    }
    $fullPath = Join-Path $repo ($Path -replace "/", "\")
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        return
    }
    $lines = Get-Content -LiteralPath $fullPath
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $line = $lines[$i]
        if ($line -match '^\s*environment\s*:') {
            throw "Workflow environment blocks are forbidden because they can create GitHub deployment records: $($Path):$($i + 1)"
        }
        if ($line -match '^\s*deployment\s*:') {
            throw "Workflow deployment keys are forbidden in this repository: $($Path):$($i + 1)"
        }
        if ($line -match 'continue-on-error\s*:\s*true') {
            throw "Workflow soft-fail behavior is forbidden in $($Path):$($i + 1)"
        }
        if ($line -match '(--exclude|skip-dirs|ignore-globs|paths-ignore).*(src|tests|tools|third_party|\.github)') {
            throw "Workflow scanner exclusion covers source-controlled code in $($Path):$($i + 1)"
        }
    }
    Assert-NoGithubContextInRunBlock -Path $Path -Lines $lines
    Assert-NoFragilePowerShellGalleryBootstrap -Path $Path -Lines $lines
}

# Purpose: Reject generated binaries introduced by the changed path set.
# Inputs: `Path` is repository-relative and may point to a deleted, new, or modified file.
# Outputs: Throws when a forbidden generated artifact is present outside allowed provenance roots.
function Test-ChangedBinaryPolicy {
    param([Parameter(Mandatory = $true)][string]$Path)

    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    if (@(".pdb", ".ilk", ".obj", ".exe", ".dll") -notcontains $extension) {
        return
    }
    if ($Path -match '^third_party/upstream/') {
        return
    }
    $fullPath = Join-Path $repo ($Path -replace "/", "\")
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        throw "Generated binary artifact is not allowed in source control: $Path"
    }
}

$jsonChangedPaths = @()
if (-not [string]::IsNullOrWhiteSpace($ChangedPathBase64)) {
    $decodedJson = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ChangedPathBase64))
    $parsedPaths = $decodedJson | ConvertFrom-Json
    foreach ($parsedPath in $parsedPaths) {
        $jsonChangedPaths += [string]$parsedPath
    }
} elseif (-not [string]::IsNullOrWhiteSpace($ChangedPathJson)) {
    $parsedPaths = $ChangedPathJson | ConvertFrom-Json
    foreach ($parsedPath in $parsedPaths) {
        $jsonChangedPaths += [string]$parsedPath
    }
}
$allChangedPaths = @($jsonChangedPaths) + @($ChangedPath) + @($RemainingChangedPath | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_) -and -not $_.StartsWith("-", [System.StringComparison]::Ordinal)
})
$paths = Get-SuperZipChangedPath -ChangedPath $allChangedPaths -BaseRef $BaseRef -HeadRef $HeadRef -IncludeUntracked:$IncludeUntracked

Push-Location $repo
try {
    if (@($ChangedPath).Count -eq 0 -and [string]::IsNullOrWhiteSpace($BaseRef)) {
        git diff --check
        if ($LASTEXITCODE -ne 0) {
            throw "git diff --check failed for unstaged changes."
        }
        git diff --cached --check
        if ($LASTEXITCODE -ne 0) {
            throw "git diff --cached --check failed for staged changes."
        }
    }
} finally {
    Pop-Location
}

foreach ($path in $paths) {
    Test-ChangedBinaryPolicy -Path $path
    Test-ChangedFileTextPolicy -Path $path
    Test-ChangedWorkflowPolicy -Path $path
}

Write-Output "Changed-file hygiene passed. Paths checked: $($paths.Count)."
