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
    (Join-Path $repo "skills"),
    (Join-Path $repo "third_party\miniz")
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

Write-Host "Security scan passed."
