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
    "\resources\design\"
)
$changedLineRangesByPath = @{}
$untrackedGitPaths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
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

    $normalized = "\" + (ConvertTo-RepoRelativePath -Path $Path).Replace("/", "\")
    # Changed vendor code has the same function gates as owned code; full cleanup inventories retain provenance scope.
    if (-not $script:AuditChangedOnly -and $normalized.StartsWith("\third_party\", [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    foreach ($fragment in $skipFragments) {
        if ($normalized.IndexOf($fragment, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

# Purpose: Read Git evidence without shell quoting, locale, or native-status ambiguity.
# Inputs: Arguments contains individual Git arguments; AllowFailure is only for revision probes.
# Outputs: Returns UTF-8 output and exit status, or throws on timeout or a required Git failure.
function Invoke-AuditGit {
    param([string[]]$Arguments, [switch]$AllowFailure)

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $start.WorkingDirectory = $repo
    $start.Arguments = ($Arguments | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join " "
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "Git exceeded the refactor audit's 30-second command deadline."
        }
        $output = $stdout.GetAwaiter().GetResult()
        $stderr.GetAwaiter().GetResult() | Out-Null
        if ($process.ExitCode -ne 0 -and -not $AllowFailure) {
            throw "Git failed during refactor auditing (exit=$($process.ExitCode), command=$($Arguments[0]))."
        }
        return [pscustomobject]@{ Output = $output; ExitCode = $process.ExitCode }
    } finally {
        $process.Dispose()
    }
}

# Purpose: Decode a NUL-delimited Git path inventory without losing Unicode or spaces.
# Inputs: Arguments selects a Git path-list command with its -z option.
# Outputs: Returns each exact repository-relative path, failing if Git cannot produce the inventory.
function Get-AuditGitPath {
    param([string[]]$Arguments)

    $result = Invoke-AuditGit -Arguments $Arguments
    return $result.Output.Split([char[]]@([char]0), [StringSplitOptions]::RemoveEmptyEntries)
}

# Purpose: Verify that a Git revision is available for changed-code auditing.
# Inputs: `Revision` is a Git revision expression.
# Outputs: Returns true when Git can resolve the revision to a commit.
function Test-GitRevisionAvailable {
    param([Parameter(Mandatory = $true)][string]$Revision)

    $result = Invoke-AuditGit -Arguments @("rev-parse", "--verify", "--quiet", "--end-of-options", "$Revision^{commit}") -AllowFailure
    return $result.ExitCode -eq 0
}

# Purpose: Select the comparison base for changed-only audit mode.
# Inputs: None; uses explicit `GitBase`, uncommitted work, or the previous commit.
# Outputs: Returns a Git base revision, or an empty string when no comparison is possible.
function Resolve-ChangedAuditBase {
    if (-not [string]::IsNullOrWhiteSpace($script:AuditGitBase)) {
        if (-not (Test-GitRevisionAvailable -Revision $script:AuditGitBase)) {
            throw "The requested refactor audit comparison revision is unavailable."
        }
        return $script:AuditGitBase
    }
    $status = Invoke-AuditGit -Arguments @("status", "--porcelain", "--untracked-files=normal")
    if ($status.Output -and (Test-GitRevisionAvailable -Revision "HEAD")) {
        return "HEAD"
    }
    if (Test-GitRevisionAvailable -Revision "HEAD~1") {
        return "HEAD~1"
    }
    return ""
}

# Purpose: Inventory actual repository source without descending into ignored workspace copies.
# Inputs: Base selects a changed-only comparison; an empty base audits all tracked and new source.
# Outputs: Returns existing source files once, retaining tracked files even when ignore rules match.
function Get-AuditSourceFile {
    param([string]$Base = "")

    $untracked = @(Get-AuditGitPath -Arguments @("ls-files", "--others", "--exclude-standard", "-z", "--"))
    foreach ($path in $untracked) {
        $script:untrackedGitPaths.Add($path) | Out-Null
    }
    if ([string]::IsNullOrEmpty($Base)) {
        $paths = @(Get-AuditGitPath -Arguments @("ls-files", "--cached", "-z", "--")) + $untracked
    } else {
        $paths = @(Get-AuditGitPath -Arguments @("diff", "--no-ext-diff", "--name-only", "--diff-filter=ACMR", "-z", $Base, "--")) + $untracked
    }
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $paths) {
        if (-not $seen.Add($path)) {
            continue
        }
        $full = Join-Path $repo $path
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
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
        [AllowEmptyString()][string]$Base,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $gitPath = ConvertTo-GitRelativePath -Path $Path
    if ([string]::IsNullOrEmpty($Base) -or $script:untrackedGitPaths.Contains($gitPath)) {
        return [pscustomobject]@{ Start = 1; End = [int]::MaxValue }
    }
    $diff = Invoke-AuditGit -Arguments @("diff", "--unified=0", "--no-ext-diff", $Base, "--", $gitPath)
    $ranges = New-Object System.Collections.Generic.List[object]
    $diffLines = @($diff.Output -split "\r?\n")
    for ($index = 0; $index -lt $diffLines.Count; ++$index) {
        $line = $diffLines[$index]
        $match = [regex]::Match($line, "^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
        if (-not $match.Success) {
            continue
        }
        $start = [int]$match.Groups[1].Value
        $length = if ($match.Groups[2].Success) { [int]$match.Groups[2].Value } else { 1 }
        if ($length -le 0) {
            # Comment-only removal belongs to the following contract/function. Code removal stays at
            # the preceding cursor, so deleting an entire function cannot implicate its unchanged neighbor.
            $commentsOnly = $true
            for ($next = $index + 1; $next -lt $diffLines.Count -and $diffLines[$next] -notmatch '^@@'; ++$next) {
                if ($diffLines[$next].StartsWith('-') -and $diffLines[$next] -notmatch '^-\s*(//|/\*|\*|#|$)') {
                    $commentsOnly = $false
                    break
                }
            }
            if ($commentsOnly) { ++$start }
            $length = 1
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
# Outputs: Requires all three nonempty fields in the immediately preceding comment block.
function Test-NearbyContractComment {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][int]$Index
    )

    $comments = [System.Collections.Generic.List[string]]::new()
    for ($i = $Index - 1; $i -ge [Math]::Max(0, $Index - 16); --$i) {
        if ([string]::IsNullOrWhiteSpace($Lines[$i])) { continue }
        if ($Lines[$i] -notmatch '^\s*(//|/\*|\*|#)') { break }
        $comments.Add($Lines[$i])
    }
    $text = $comments -join "`n"
    return $text -match 'Purpose:\s*\S' -and $text -match 'Inputs?:\s*\S' -and $text -match 'Outputs?:\s*\S'
}

# Purpose: Mask C/C++ comments and literals while preserving source lines and comment identities.
# Inputs: Complete source text, including raw strings and multiline comments.
# Outputs: Code-only lines and distinct comment line numbers for bounded function auditing.
function Get-CppAuditProjection {
    param([string]$Text)

    $pattern = '(?<Comment>/\*[\s\S]*?\*/|//[^\r\n]*)|\bR"(?<Raw>[^ ()\\\r\n]{0,16})\([\s\S]*?\)\k<Raw>"|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    $commentLines = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($match in [regex]::Matches($Text, $pattern)) {
        if (-not $match.Groups['Comment'].Success) { continue }
        $startLine = ([regex]::Matches($Text.Substring(0, $match.Index), "`n")).Count
        $length = ([regex]::Matches($match.Value, "`n")).Count + 1
        for ($line = $startLine; $line -lt $startLine + $length; ++$line) {
            $commentLines.Add($line) | Out-Null
        }
    }
    $masked = [regex]::Replace($Text, $pattern, [Text.RegularExpressions.MatchEvaluator]{
        param($match)
        return [regex]::Replace($match.Value, '[^\r\n]', ' ')
    })
    return [pscustomobject]@{ Lines = @($masked -split '\r?\n'); CommentLines = $commentLines }
}

# Purpose: Locate a C/C++ definition's signature for either same-line or next-line opening braces.
# Inputs: Masked source lines and candidate brace line; control blocks and test registrations are excluded.
# Outputs: Signature start index, or -1 when the candidate is not a recognized function definition.
function Get-CppFunctionStart {
    param([string[]]$Lines, [int]$Index)

    $candidate = $Lines[$Index].Trim()
    if ($candidate -notmatch '\{\s*$') { return -1 }
    $start = $Index
    if ($candidate -eq '{' -or $candidate -notmatch '\b[A-Za-z_]\w*\s*\(') {
        for ($i = $Index - 1; $i -ge [Math]::Max(0, $Index - 12); --$i) {
            $line = $Lines[$i].Trim()
            if (-not $line) { continue }
            if ($line -match '[;{}]') { break }
            $start = $i
            if ($line -match '\b[A-Za-z_]\w*\s*\(') { break }
        }
    }
    if ($start -gt 0 -and $Lines[$start - 1].Trim() -eq 'extern') {
        # The projection masks the linkage string; keep a separate extern "C"
        # line with its declaration so the preceding contract remains adjacent.
        --$start
    }
    $signature = (($Lines[$start..$Index] -join ' ').Trim())
    if ($signature -match '^(if|else|for|while|switch|catch|TEST_CASE)\b' -or
        $signature -match '\]\s*\(' -or $signature -match '(^|\s)(==|!=|<=|>=|&&|\|\|)(\s|$)' -or
        $signature -notmatch '\b[A-Za-z_]\w*\s*\([^;{}]*\)\s*(const\s*)?(noexcept\s*)?(->[^{}]*)?\{\s*$') {
        return -1
    }
    return $start
}

# Purpose: Include the immediately preceding contract in a changed function's finding range.
# Inputs: Raw lines and signature start; preceding executable statements terminate the range.
# Outputs: First contract/blank line index, bounded to the sixteen-line documentation window.
function Get-CppContractStart {
    param([string[]]$Lines, [int]$Index)

    $start = $Index
    for ($i = $Index - 1; $i -ge [Math]::Max(0, $Index - 16); --$i) {
        if ([string]::IsNullOrWhiteSpace($Lines[$i])) { continue }
        if ($Lines[$i] -notmatch '^\s*(//|/\*|\*|#|$)') { break }
        $start = $i
    }
    return $start
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
    $projection = Get-CppAuditProjection -Text ([IO.File]::ReadAllText($Path))
    $code = $projection.Lines
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $signatureStart = Get-CppFunctionStart -Lines $code -Index $i
        if ($signatureStart -lt 0) { continue }
        $findingStart = (Get-CppContractStart -Lines $lines -Index $signatureStart) + 1
        $depth = 0
        $body = New-Object System.Collections.Generic.List[string]
        for ($j = $i; $j -lt $lines.Count; ++$j) {
            $body.Add($code[$j])
            $depth += Get-BraceDelta -Line $code[$j]
            if ($depth -eq 0) {
                break
            }
        }
        $lineCount = $j - $signatureStart + 1
        $markers = Measure-ComplexityMarker -Lines $body.ToArray()
        if ($lineCount -gt $script:AuditMaxFunctionLines) {
            Add-RefactorFinding -Findings $Findings -Category "large-function" -Path $Path -Line $findingStart -EndLine ($j + 1) -Detail "$lineCount lines"
        }
        if ($markers -gt $script:AuditMaxComplexityMarkers) {
            Add-RefactorFinding -Findings $Findings -Category "complex-function" -Path $Path -Line $findingStart -EndLine ($j + 1) -Detail "$markers markers"
        }
        if ($script:AuditCheckContracts -and -not (Test-NearbyContractComment -Lines $lines -Index $signatureStart)) {
            Add-RefactorFinding -Findings $Findings -Category "missing-contract" -Path $Path -Line $findingStart -EndLine ($j + 1) -Detail "No nearby Purpose/Input/Output contract comment"
        }
        if ($script:AuditCheckContracts -and $lineCount -ge 100) {
            $commentCount = @($signatureStart..$j | Where-Object { $projection.CommentLines.Contains($_) }).Count
            if ($commentCount * 100 -lt $lineCount * 2) {
                Add-RefactorFinding -Findings $Findings -Category "poor-documentation" -Path $Path -Line $findingStart -EndLine ($j + 1) -Detail "Long function lacks parsing/ownership/invariant comments"
            }
        }
        $i = $j
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
    $files = @(Get-AuditSourceFile -Base $base)
    foreach ($file in $files) {
        $relative = ConvertTo-RepoRelativePath -Path $file.FullName
        $script:changedLineRangesByPath[$relative] = @(Get-ChangedLineRange -Base $base -Path $file.FullName)
    }
} else {
    $files = @(Get-AuditSourceFile)
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
