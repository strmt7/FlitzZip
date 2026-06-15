param(
    [int]$MaxFileLines = 1200,
    [int]$MaxFunctionLines = 180,
    [int]$MaxComplexityMarkers = 45,
    [switch]$CheckContracts,
    [switch]$FailOnFindings
)

# Purpose: Audit SuperZip source files for refactoring candidates without modifying the tree.
# Inputs: Threshold parameters define large-file, large-function, and complexity-marker limits; `CheckContracts` enables the noisy contract-comment heuristic.
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

# Purpose: Count brace delta in a source line for approximate function-size tracking.
# Inputs: `Line` is a single source line.
# Outputs: Returns open-brace count minus close-brace count.
function Get-BraceDelta {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Line)

    $delta = 0
    foreach ($char in $Line.ToCharArray()) {
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
function Measure-ComplexityMarkers {
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

    $start = [Math]::Max(0, $Index - 4)
    for ($i = $start; $i -lt $Index; ++$i) {
        if ($Lines[$i].Contains("Purpose:")) {
            return $true
        }
    }
    return $false
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
        [Parameter(Mandatory = $true)][string]$Detail
    )

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
        if ($trimmed -match "^(if|for|while|switch|catch)\b") {
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
        $markers = Measure-ComplexityMarkers -Lines $body.ToArray()
        if ($lineCount -gt $MaxFunctionLines) {
            Add-RefactorFinding -Findings $Findings -Category "large-function" -Path $Path -Line ($i + 1) -Detail "$lineCount lines"
        }
        if ($markers -gt $MaxComplexityMarkers) {
            Add-RefactorFinding -Findings $Findings -Category "complex-function" -Path $Path -Line ($i + 1) -Detail "$markers markers"
        }
        if ($CheckContracts -and -not (Test-NearbyContractComment -Lines $lines -Index $i)) {
            Add-RefactorFinding -Findings $Findings -Category "missing-contract" -Path $Path -Line ($i + 1) -Detail "No nearby Purpose/Input/Output contract comment"
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
        $markers = Measure-ComplexityMarkers -Lines $body.ToArray()
        if ($lineCount -gt $MaxFunctionLines) {
            Add-RefactorFinding -Findings $Findings -Category "large-function" -Path $Path -Line ($i + 1) -Detail "$lineCount lines"
        }
        if ($markers -gt $MaxComplexityMarkers) {
            Add-RefactorFinding -Findings $Findings -Category "complex-function" -Path $Path -Line ($i + 1) -Detail "$markers markers"
        }
    }
}

$findings = [System.Collections.Generic.List[object]]::new()
$files = Get-ChildItem -LiteralPath $repo -Recurse -File |
    Where-Object { $sourceExtensions -contains $_.Extension.ToLowerInvariant() } |
    Where-Object { -not (Test-SkippedPath -Path $_.FullName) }

foreach ($file in $files) {
    $lines = [IO.File]::ReadAllLines($file.FullName)
    if ($lines.Count -gt $MaxFileLines) {
        Add-RefactorFinding -Findings $findings -Category "large-file" -Path $file.FullName -Line 1 -Detail "$($lines.Count) lines"
    }
    if ($file.Extension -eq ".ps1") {
        Test-PowerShellFile -Path $file.FullName -Findings $findings
    } else {
        Test-CppFile -Path $file.FullName -Findings $findings
    }
}

if ($findings.Count -eq 0) {
    Write-Host "refactor_audit status=clean"
    return
}

foreach ($finding in ($findings | Sort-Object Path, Line, Category)) {
    Write-Host ("refactor_finding category={0} path=""{1}"" line={2} detail=""{3}""" -f $finding.Category, $finding.Path, $finding.Line, $finding.Detail)
}
Write-Host "refactor_audit status=findings count=$($findings.Count)"
if ($FailOnFindings) {
    exit 1
}
