#Requires -Version 7.0

# Purpose: Print the end of a verbose Windows Installer log for actionable CI failures.
# Inputs: Path is the log file to read; Label identifies the install phase in output.
# Outputs: Writes a bounded log tail to stdout and returns no value.
function Write-MsiLogTail {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )
    if (Test-Path -LiteralPath $Path) {
        Write-Information -InformationAction Continue "----- $Label log tail -----"
        Get-Content -LiteralPath $Path -Tail 160
        Write-Information -InformationAction Continue "----- end $Label log tail -----"
    } else {
        Write-Information -InformationAction Continue "$Label log was not produced at $Path"
    }
}

# Purpose: Run msiexec with a short bounded wait and fail with the matching verbose log tail.
# Inputs: Phase names the operation, Arguments are passed to msiexec.exe, LogPath is the verbose log location,
# and TimeoutSeconds bounds stale installer/elevation waits.
# Outputs: Returns no value when msiexec exits 0; throws with diagnostics on timeout or non-zero exit code.
function Invoke-CheckedMsiExec {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Phase,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$LogPath,
        [ValidateRange(1, 1800)]
        [int]$TimeoutSeconds = 300
    )
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path ([Environment]::SystemDirectory) 'msiexec.exe'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    # Let .NET serialize native arguments; quoting switches changes MSI parsing.
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            if (-not $process.HasExited) { $process.Kill() }
            [void]$process.WaitForExit(5000)
            Write-MsiLogTail -Path $LogPath -Label $Phase
            throw ("$Phase timed out after $TimeoutSeconds seconds. " +
                "Check whether an installer or elevation prompt was left unanswered.")
        }
        if ($process.ExitCode -ne 0) {
            Write-MsiLogTail -Path $LogPath -Label $Phase
            throw "$Phase failed with exit code $($process.ExitCode)."
        }
    } finally { $process.Dispose() }
}

Export-ModuleMember -Function Invoke-CheckedMsiExec
