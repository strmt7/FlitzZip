# Purpose: Set or remove one process-local environment value without conflating absent and empty values.
# Inputs: Name is a valid Windows environment key; Value is a string or null for removal.
# Outputs: Changes only this process; emits no output and preserves empty strings where the host supports them.
function Set-SuperZipProcessEnvironmentValue {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([Parameter(Mandatory = $true)][ValidatePattern('\A[^\x00=]+\z')][string]$Name,
        [AllowNull()][object]$Value)
    if (-not $PSCmdlet.ShouldProcess("Process environment variable $Name", 'Apply process-local value')) { return }
    if ($null -eq $Value) {
        if (Test-Path -LiteralPath "Env:$Name") { Remove-Item -LiteralPath "Env:$Name" -ErrorAction Stop }
    } else {
        [Environment]::SetEnvironmentVariable($Name, [string]$Value, 'Process')
    }
}

# Purpose: Scope compiler or build settings while restoring the caller's exact process environment.
# Inputs: Settings contains valid environment keys and string/null values; Action is trusted repository work.
# Outputs: Forwards action output/errors; restores absent, empty and populated values after success or failure.
function Invoke-SuperZipProcessEnvironment {
    param([Parameter(Mandatory = $true)][Collections.IDictionary]$Settings,
        [Parameter(Mandatory = $true)][scriptblock]$Action)
    $previousEnvironmentValues = @{}
    foreach ($environmentKey in $Settings.Keys) {
        if ($environmentKey -isnot [string] -or $environmentKey -notmatch '\A[^\x00=]+\z') {
            throw 'Process environment settings require nonempty names without NUL or equals signs.'
        }
        $previousEnvironmentValues[$environmentKey] = [Environment]::GetEnvironmentVariable($environmentKey, 'Process')
    }
    try {
        foreach ($environmentKey in $Settings.Keys) {
            Set-SuperZipProcessEnvironmentValue -Name $environmentKey -Value $Settings[$environmentKey] -Confirm:$false -WhatIf:$false
        }
        & $Action
    } finally {
        foreach ($environmentKey in $previousEnvironmentValues.Keys) {
            Set-SuperZipProcessEnvironmentValue -Name $environmentKey -Value $previousEnvironmentValues[$environmentKey] -Confirm:$false -WhatIf:$false
        }
    }
}
