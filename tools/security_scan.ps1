$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$secretPatterns = @(
    "ghp_[A-Za-z0-9_]{30,}",
    "github_pat_[A-Za-z0-9_]+",
    "sk-[A-Za-z0-9]{20,}",
    "AKIA[0-9A-Z]{16}"
)

$excludedRoots = @(
    "\.git\",
    "\build\",
    "\.vs\",
    "\third_party\miniz\"
)

$files = Get-ChildItem -Path $repo -Recurse -File -Force | Where-Object {
    $path = $_.FullName
    -not ($excludedRoots | Where-Object { $path.Contains($_) })
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
    -not $_.FullName.Contains("\build\")
}
if ($forbidden) {
    throw "Build artifacts found outside build directory: $($forbidden[0].FullName)"
}

Write-Host "Security scan passed."
