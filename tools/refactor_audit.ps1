param(
    [int]$MaxFileLines = 1200,
    [int]$MaxFunctionLines = 180,
    [int]$MaxComplexityMarkers = 45,
    [switch]$ChangedOnly,
    [string]$GitBase = "",
    [switch]$CheckContracts,
    [switch]$FailOnFindings
)

# Purpose: Audit SuperZip source files for refactoring candidates without modifying the tree.
# Inputs: Threshold parameters define large-file, large-function, and complexity-marker limits; `ChangedOnly` limits hard gates to edited line ranges; `CheckContracts` enables the noisy contract-comment heuristic.
# Outputs: Prints parseable findings and exits nonzero only when `FailOnFindings` is set and findings exist.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sourceExtensions = @(".c", ".cc", ".cpp", ".h", ".hpp", ".ps1")
$skipFragments = @(
    "\.git\",
    "\build\",
    "\out\",
    "\third_party\",
    "\resources\design\"
)
$changedLineRangesByPath = @{}
$script:AuditMaxFileLines = $MaxFileLines
$script:AuditMaxFunctionLines = $MaxFunctionLines
$script:AuditMaxComplexityMarkers = $MaxComplexityMarkers
$script:AuditChangedOnly = $ChangedOnly
$script:AuditGitBase = $GitBase
$script:AuditCheckContracts = $CheckContracts
$script:AuditFailOnFindings = $FailOnFindings

# Purpose: Return a repository-relative path for stable audit output.
# Inputs: `Path` is an absolute or relative filesystem path.
# Outputs: Returns a path relative to the repository root when possible.
function ConvertTo-RepoRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($repo).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($root.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    }
    return $full
}

# Purpose: Convert a filesystem path to a repository-relative Git path.
# Inputs: `Path` is a source-controlled path under the repository root.
# Outputs: Returns a slash-separated path suitable for `git diff` arguments.
function ConvertTo-GitRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (ConvertTo-RepoRelativePath -Path $Path).Replace("\", "/")
}

# Purpose: Decide whether an audit path is generated, vendored, or intentionally outside refactoring scope.
# Inputs: `Path` is an absolute filesystem path.
# Outputs: Returns true for skipped paths.
function Test-SkippedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $normalized = [IO.Path]::GetFullPath($Path)
    foreach ($fragment in $skipFragments) {
        if ($normalized.IndexOf($fragment, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

# Purpose: Verify that a Git revision is available for changed-code auditing.
# Inputs: `Revision` is a Git revision expression.
# Outputs: Returns true when Git can resolve the revision to a commit.
function Test-GitRevisionAvailable {
    param([Parameter(Mandatory = $true)][string]$Revision)

    & git -C $repo rev-parse --verify "$Revision^{commit}" *> $null
    return $LASTEXITCODE -eq 0
}

# Purpose: Select the comparison base for changed-only audit mode.
# Inputs: None; uses explicit `GitBase`, uncommitted work, or the previous commit.
# Outputs: Returns a Git base revision, or an empty string when no comparison is possible.
function Resolve-ChangedAuditBase {
    if (-not [string]::IsNullOrWhiteSpace($script:AuditGitBase)) {
        return $script:AuditGitBase
    }
    $status = & git -C $repo status --porcelain --untracked-files=no
    if ($LASTEXITCODE -eq 0 -and $status) {
        return "HEAD"
    }
    if (Test-GitRevisionAvailable -Revision "HEAD~1") {
        return "HEAD~1"
    }
    return ""
}

# Purpose: Return source files changed relative to a Git base.
# Inputs: `Base` is a resolved Git comparison revision.
# Outputs: Returns filesystem paths for changed source files still present in the working tree.
function Get-ChangedSourceFile {
    param([Parameter(Mandatory = $true)][string]$Base)

    $paths = & git -C $repo diff --name-only --diff-filter=ACMR $Base --
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enumerate changed files for refactor audit against $Base."
    }
    foreach ($path in $paths) {
        $full = Join-Path $repo $path
        if (-not (Test-Path -LiteralPath $full)) {
            continue
        }
        $extension = [IO.Path]::GetExtension($full).ToLowerInvariant()
        if ($sourceExtensions -contains $extension -and -not (Test-SkippedPath -Path $full)) {
            Get-Item -LiteralPath $full
        }
    }
}

# Purpose: Return added/modified line ranges for one changed file.
# Inputs: `Base` is the Git comparison revision and `Path` is an existing source file.
# Outputs: Returns one-based inclusive line ranges in the new file.
function Get-ChangedLineRange {
    param(
        [Parameter(Mandatory = $true)][string]$Base,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $gitPath = ConvertTo-GitRelativePath -Path $Path
    $diff = & git -C $repo diff --unified=0 --no-ext-diff $Base -- $gitPath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect changed line ranges for $gitPath against $Base."
    }
    $ranges = New-Object System.Collections.Generic.List[object]
    foreach ($line in $diff) {
        $match = [regex]::Match($line, "^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
        if (-not $match.Success) {
            continue
        }
        $start = [int]$match.Groups[1].Value
        $length = if ($match.Groups[2].Success) { [int]$match.Groups[2].Value } else { 1 }
        if ($length -le 0) {
            continue
        }
        $ranges.Add([pscustomobject]@{
            Start = $start
            End = $start + $length - 1
        })
    }
    return $ranges.ToArray()
}

# Purpose: Decide whether an audit finding intersects edited lines in changed-only mode.
# Inputs: `Path`, `StartLine`, and `EndLine` describe the candidate finding range.
# Outputs: Returns true for full-audit mode or when an edited line falls inside the range.
function Test-FindingChangedLineIntersection {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$StartLine,
        [Parameter(Mandatory = $true)][int]$EndLine
    )

    if (-not $script:AuditChangedOnly) {
        return $true
    }
    $relative = ConvertTo-RepoRelativePath -Path $Path
    $ranges = $script:changedLineRangesByPath[$relative]
    if ($null -eq $ranges) {
        return $false
    }
    foreach ($range in $ranges) {
        if ($EndLine -ge $range.Start -and $StartLine -le $range.End) {
            return $true
        }
    }
    return $false
}

# Purpose: Count brace delta in a source line for approximate function-size tracking.
# Inputs: `Line` is a single source line.
# Outputs: Returns open-brace count minus close-brace count.
function Get-BraceDelta {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Line)

    $delta = 0
    $inSingleQuote = $false
    $inDoubleQuote = $false
    $escaped = $false
    foreach ($char in $Line.ToCharArray()) {
        if ($escaped) {
            $escaped = $false
            continue
        }
        if ($char -eq [char]92 -and $inDoubleQuote) {
            $escaped = $true
            continue
        }
        if ($char -eq [char]39 -and -not $inDoubleQuote) {
            $inSingleQuote = -not $inSingleQuote
            continue
        }
        if ($char -eq [char]34 -and -not $inSingleQuote) {
            $inDoubleQuote = -not $inDoubleQuote
            continue
        }
        if ($inSingleQuote -or $inDoubleQuote) {
            continue
        }
        if ($char -eq "{") {
            ++$delta
        } elseif ($char -eq "}") {
            --$delta
        }
    }
    return $delta
}

# Purpose: Count simple branch and boolean markers as a rough complexity signal.
# Inputs: `Lines` is a function body slice.
# Outputs: Returns a non-authoritative marker count for refactoring triage.
function Measure-ComplexityMarker {
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines)

    $count = 0
    foreach ($line in $Lines) {
        $count += ([regex]::Matches($line, "\b(if|for|while|case|catch)\b")).Count
        $count += ([regex]::Matches($line, "&&|\|\|")).Count
    }
    return $count
}

# Purpose: Check whether a function has a nearby SuperZip contract comment.
# Inputs: `Lines` is the full file and `Index` is the zero-based function start line.
# Outputs: Returns true when a nearby previous line contains `Purpose:`.
function Test-NearbyContractComment {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][int]$Index
    )

    $start = [Math]::Max(0, $Index - 16)
    for ($i = $start; $i -lt $Index; ++$i) {
        if ($Lines[$i].Contains("Purpose:")) {
            return $true
        }
    }
    return $false
}

# Purpose: Return the first line of a multi-line C++ statement for heuristic classification.
# Inputs: `Lines` is the full file and `Index` is the candidate line ending in an opening brace.
# Outputs: Returns the closest logical statement-start text used to distinguish functions from control blocks.
function Get-CppStatementStartText {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][int]$Index
    )

    $start = [Math]::Max(0, $Index - 8)
    for ($i = $Index; $i -ge $start; --$i) {
        $candidate = $Lines[$i].Trim()
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if ($candidate -match "^(if|else\s+if|for|while|switch|catch)\b") {
            return $candidate
        }
        if ($i -lt $Index -and $candidate -match "[;{}]\s*$") {
            break
        }
    }
    return $Lines[$Index].Trim()
}

# Purpose: Add one audit finding to the shared collection.
# Inputs: `Findings` is the mutable finding list and the remaining arguments describe the finding.
# Outputs: Appends a structured object to `Findings`.
function Add-RefactorFinding {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Findings,
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Line,
        [int]$EndLine = 0,
        [Parameter(Mandatory = $true)][string]$Detail
    )

    if ($EndLine -le 0) {
        $EndLine = $Line
    }
    if (-not (Test-FindingChangedLineIntersection -Path $Path -StartLine $Line -EndLine $EndLine)) {
        return
    }
    $Findings.Add([pscustomobject]@{
        Category = $Category
        Path = ConvertTo-RepoRelativePath -Path $Path
        Line = $Line
        Detail = $Detail
    })
}

# Purpose: Audit C/C++ files for large or undocumented function bodies.
# Inputs: `Path` is one source file and `Findings` is the mutable output list.
# Outputs: Adds findings for large functions, high marker counts, and missing nearby contracts.
function Test-CppFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Findings
    )

    $lines = [IO.File]::ReadAllLines($Path)
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $trimmed = $lines[$i].Trim()
        if ($trimmed -notmatch "\)\s*(const\s*)?(\{|noexcept\s*\{|->.*\{)$") {
            continue
        }
        $statementStart = Get-CppStatementStartText -Lines $lines -Index $i
        if ($statementStart -match "^(if|else|for|while|switch|catch|TEST_CASE)\b" -or
            $trimmed -match "^\}\s*(else|catch)\b" -or
            $trimmed -match "\]\s*\(" -or
            $trimmed -match "(\s(==|!=|<=|>=|<|>)\s|&&|\|\|)") {
            continue
        }
        $depth = 0
        $body = New-Object System.Collections.Generic.List[string]
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $body.Add($lines[$j])
            $depth += Get-BraceDelta -Line $lines[$j]
            if ($depth -eq 0) {
                break
            }
        }
        $lineCount = $body.Count
        $markers = Measure-ComplexityMarker -Lines $body.ToArray()
        if ($lineCount -gt $script:AuditMaxFunctionLines) {
            Add-RefactorFinding -Findings $Findings -Category "large-function" -Path $Path -Line ($i + 1) -EndLine ($j + 1) -Detail "$lineCount lines"
        }
        if ($markers -gt $script:AuditMaxComplexityMarkers) {
            Add-RefactorFinding -Findings $Findings -Category "complex-function" -Path $Path -Line ($i + 1) -EndLine ($j + 1) -Detail "$markers markers"
        }
        if ($script:AuditCheckContracts -and -not (Test-NearbyContractComment -Lines $lines -Index $i)) {
            Add-RefactorFinding -Findings $Findings -Category "missing-contract" -Path $Path -Line ($i + 1) -EndLine ($j + 1) -Detail "No nearby Purpose/Input/Output contract comment"
        }
    }
}

# Purpose: Audit PowerShell files for large functions.
# Inputs: `Path` is one script and `Findings` is the mutable output list.
# Outputs: Adds findings for large PowerShell functions and high marker counts.
function Test-PowerShellFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Findings
    )

    $lines = [IO.File]::ReadAllLines($Path)
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -notmatch "^\s*function\s+[\w-]+\s*\{?") {
            continue
        }
        $depth = 0
        $body = New-Object System.Collections.Generic.List[string]
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $body.Add($lines[$j])
            $depth += Get-BraceDelta -Line $lines[$j]
            if ($depth -eq 0) {
                break
            }
        }
        $lineCount = $body.Count
        $markers = Measure-ComplexityMarker -Lines $body.ToArray()
        if ($lineCount -gt $script:AuditMaxFunctionLines) {
            Add-RefactorFinding -Findings $Findings -Category "large-function" -Path $Path -Line ($i + 1) -EndLine ($j + 1) -Detail "$lineCount lines"
        }
        if ($markers -gt $script:AuditMaxComplexityMarkers) {
            Add-RefactorFinding -Findings $Findings -Category "complex-function" -Path $Path -Line ($i + 1) -EndLine ($j + 1) -Detail "$markers markers"
        }
    }
}

$findings = [System.Collections.Generic.List[object]]::new()
if ($script:AuditChangedOnly) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git is required for changed-only refactor auditing."
    }
    $base = Resolve-ChangedAuditBase
    if ([string]::IsNullOrWhiteSpace($base)) {
        Write-Output "refactor_audit status=clean mode=changed-only reason=no-comparison-base"
        return
    }
    $files = @(Get-ChangedSourceFile -Base $base)
    foreach ($file in $files) {
        $relative = ConvertTo-RepoRelativePath -Path $file.FullName
        $script:changedLineRangesByPath[$relative] = @(Get-ChangedLineRange -Base $base -Path $file.FullName)
    }
} else {
    $files = Get-ChildItem -LiteralPath $repo -Recurse -File |
        Where-Object { $sourceExtensions -contains $_.Extension.ToLowerInvariant() } |
        Where-Object { -not (Test-SkippedPath -Path $_.FullName) }
}

foreach ($file in $files) {
    $lines = [IO.File]::ReadAllLines($file.FullName)
    if ($lines.Count -gt $script:AuditMaxFileLines) {
        Add-RefactorFinding -Findings $findings -Category "large-file" -Path $file.FullName -Line 1 -Detail "$($lines.Count) lines"
    }
    if ($file.Extension -eq ".ps1") {
        Test-PowerShellFile -Path $file.FullName -Findings $findings
    } else {
        Test-CppFile -Path $file.FullName -Findings $findings
    }
}

if ($findings.Count -eq 0) {
    Write-Output "refactor_audit status=clean"
    return
}

foreach ($finding in ($findings | Sort-Object Path, Line, Category)) {
    Write-Output ("refactor_finding category={0} path=""{1}"" line={2} detail=""{3}""" -f $finding.Category, $finding.Path, $finding.Line, $finding.Detail)
}
Write-Output "refactor_audit status=findings count=$($findings.Count)"
if ($script:AuditFailOnFindings) {
    exit 1
}
