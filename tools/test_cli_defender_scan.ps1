param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$repo = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $repo "build\$Configuration\superzip_cli.exe"
$source = Join-Path $repo "README.md"
if (-not (Test-Path -LiteralPath $cli)) {
    throw "CLI binary not found. Run tools/build.ps1 first."
}

$nonce = [guid]::NewGuid().ToString('N')
$control = Join-Path $env:TEMP "superzip-defender-control-$nonce.zip"
$flagged = Join-Path $env:TEMP "superzip-defender-flagged-$nonce.zip"
if ((Test-Path -LiteralPath $control) -or (Test-Path -LiteralPath $flagged)) {
    throw "Defender test output path already exists."
}

try {
    $controlOutput = @(& $cli compress --format zip --output $control $source)
    $controlExit = $LASTEXITCODE
    if ($controlExit -ne 0 -or ($controlOutput -join "`n") -notmatch '(?m)^entries=1\b' -or
        ($controlOutput -join "`n") -match '(?m)^defender_') {
        throw "Unflagged compression changed its output or exit behavior."
    }

    $flaggedOutput = @(& $cli compress --format zip --output $flagged --defender-scan $source)
    $flaggedExit = $LASTEXITCODE
    $text = $flaggedOutput -join "`n"
    $fields = @{}
    foreach ($line in $flaggedOutput) {
        if ($line -match '^defender_(attempted|clean|timed_out|exit_code)=(.+)$') {
            $fields[$Matches[1]] = $Matches[2]
        }
    }
    if ($fields.Count -ne 4) {
        throw "Flagged compression did not report all Defender scan fields."
    }
    $clean = $fields.attempted -eq 'true' -and $fields.clean -eq 'true' -and
        $fields.timed_out -eq 'false' -and $fields.exit_code -eq '0'
    if (($clean -and $flaggedExit -ne 0) -or (-not $clean -and $flaggedExit -eq 0)) {
        throw "Flagged compression exit code disagrees with Defender scan state."
    }
    if ($clean -and $text -notmatch '(?m)^entries=1\b') {
        throw "A clean Defender scan did not publish compression statistics."
    }
    if (-not $clean -and $text -match '(?m)^entries=1\b') {
        throw "A failed Defender scan published success statistics."
    }

    Write-Output "cli_defender_scan status=passed attempted=$($fields.attempted) clean=$($fields.clean)"
} finally {
    Remove-Item -LiteralPath $control, $flagged -ErrorAction SilentlyContinue
}
