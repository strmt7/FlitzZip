$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$ownedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out/verification-runner-tests'))
$fixtureRoot = Join-Path $ownedRoot ([guid]::NewGuid().ToString('N'))
$fixtureTools = Join-Path $fixtureRoot 'tools'
New-Item -ItemType Directory -Path $fixtureTools -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify_changes.ps1') -Destination $fixtureTools
$moduleSource = @'
# Purpose: Provide a controlled plan for runner integration fixtures.
# Inputs: The production runner's planning arguments. Outputs: Two required commands and one manual command.
function Get-SuperZipVerificationPlan {
    param($ChangedPath, $BaseRef, $HeadRef, $IncludeUntracked, $SuspectGlobalBug, $Checkpoint)
    Add-Content -LiteralPath (Join-Path $PSScriptRoot '../calls.txt') -Value "plan:$SuspectGlobalBug"
    return [pscustomobject]@{
        requiredLocalCommands = @([pscustomobject]@{id='first'}, [pscustomobject]@{id='second'})
        manualLocalCommands = @([pscustomobject]@{id='manual'})
    }
}
# Purpose: Record fixture execution and optionally fail one selected command.
# Inputs: Command descriptor and owned failure marker. Outputs: Journal entry or the original fixture error.
function Invoke-SuperZipVerificationCommand {
    param($Command)
    Add-Content -LiteralPath (Join-Path $PSScriptRoot '../calls.txt') -Value "command:$($Command.id)"
    $failure = Join-Path $PSScriptRoot '../fail.txt'
    if ((Test-Path -LiteralPath $failure) -and
        ([string](Get-Content -LiteralPath $failure -Raw)).Trim() -eq $Command.id) {
        throw "intentional fixture failure:$($Command.id)"
    }
}
Export-ModuleMember -Function Get-SuperZipVerificationPlan, Invoke-SuperZipVerificationCommand
'@
[IO.File]::WriteAllText((Join-Path $fixtureTools 'superzip_verification.psm1'), $moduleSource)

# Purpose: Exercise the real runner with controlled plans and failures without running product checks.
# Inputs: Failure id, optional manual lane, and the exact expected invocation sequence.
# Outputs: Requires one plan, immediate failure propagation and no unrelated or repeated commands.
function Assert-RunnerCase {
    param([string]$Failure, [switch]$Manual, [switch]$ExplicitFull,
          [switch]$LegacyFlag, [string[]]$Expected)
    $calls = Join-Path $fixtureRoot 'calls.txt'
    $failurePath = Join-Path $fixtureRoot 'fail.txt'
    Remove-Item -LiteralPath $calls -Force -ErrorAction SilentlyContinue
    if ($Failure) { [IO.File]::WriteAllText($failurePath, $Failure) }
    else { Remove-Item -LiteralPath $failurePath -Force -ErrorAction SilentlyContinue }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Command powershell.exe -ErrorAction Stop).Source
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' +
        (Join-Path $fixtureTools 'verify_changes.ps1') + '" -ChangedPath tools/fixture.ps1 -Checkpoint intermediate'
    if ($Manual.IsPresent) { $start.Arguments += ' -IncludeManual' }
    if ($ExplicitFull.IsPresent) { $start.Arguments += ' -Full' }
    if ($LegacyFlag.IsPresent) { $start.Arguments += ' -NoAutoEscalate' }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            throw 'Controlled verification runner timed out.'
        }
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if (($Failure -and $process.ExitCode -eq 0) -or
            (-not $Failure -and $process.ExitCode -ne 0)) {
            $trace = @(Get-Content -LiteralPath $calls) -join ', '
            throw "Unexpected runner exit code $($process.ExitCode), calls $trace`: $output"
        }
        if ($Failure -and -not $output.Contains("intentional fixture failure:$Failure")) {
            throw 'Runner lost the original failure.'
        }
        $actual = @(Get-Content -LiteralPath $calls)
        if (($actual -join '|') -ne ($Expected -join '|')) {
            throw "Unexpected verification sequence: $($actual -join ', ')"
        }
    } finally { $process.Dispose() }
}

try {
    Assert-RunnerCase -Failure first -Expected @('plan:False', 'command:first')
    Assert-RunnerCase -Failure second -Expected @('plan:False', 'command:first', 'command:second')
    Assert-RunnerCase -Manual -Failure manual -Expected @('plan:False', 'command:first', 'command:second', 'command:manual')
    Assert-RunnerCase -Expected @('plan:False', 'command:first', 'command:second')
    Assert-RunnerCase -ExplicitFull -Failure first -Expected @('plan:True', 'command:first')
    Assert-RunnerCase -LegacyFlag -Expected @('plan:False', 'command:first', 'command:second')
    Write-Output 'Verification runner focused failure/success contracts passed.'
} finally {
    $safePrefix = $ownedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not [IO.Path]::GetFullPath($fixtureRoot).StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove an unverified runner fixture root.'
    }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
