$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_environment.ps1')
$name = 'SUPERZIP_ENVIRONMENT_TEST_' + [guid]::NewGuid().ToString('N')
$otherName = $name + '_OTHER'
try {
    foreach ($initial in @($null, '', 'caller value')) {
        Set-SuperZipProcessEnvironmentValue -Name $name -Value $initial
        $expected = [Environment]::GetEnvironmentVariable($name, 'Process')
        foreach ($failAction in @($false, $true)) {
            $caught = $false
            try {
                $result = @(Invoke-SuperZipProcessEnvironment -Settings @{ $name = 'temporary'; $otherName = 'child' } -Action {
                    if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne 'temporary' -or
                        [Environment]::GetEnvironmentVariable($otherName, 'Process') -cne 'child') {
                        throw 'Scoped environment was not applied.'
                    }
                    $nested = Invoke-SuperZipProcessEnvironment -Settings @{ $name = $null } -Action {
                        if ($null -ne [Environment]::GetEnvironmentVariable($name, 'Process')) {
                            throw 'Nested scope failed to remove the environment value.'
                        }
                        19
                    }
                    if ($nested -ne 19 -or [Environment]::GetEnvironmentVariable($name, 'Process') -cne 'temporary') {
                        throw 'Nested scope failed to restore the enclosing value or output.'
                    }
                    if ($failAction) { throw 'test: scoped failure' }
                    42
                })
                if ($result.Count -ne 1 -or $result[0] -ne 42) { throw 'Scoped environment contaminated action output.' }
            } catch {
                if ($_.Exception.Message -cne 'test: scoped failure') { throw }
                $caught = $true
            }
            if ($caught -ne $failAction -or [Environment]::GetEnvironmentVariable($name, 'Process') -cne $expected -or
                $null -ne [Environment]::GetEnvironmentVariable($otherName, 'Process')) {
                throw 'Scoped environment failed exact restoration after success or failure.'
            }
        }
    }
    $mutableSettings = @{ $name = 'temporary'; $otherName = 'child' }
    Invoke-SuperZipProcessEnvironment -Settings $mutableSettings -Action { $mutableSettings.Clear() }
    if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $expected -or
        $null -ne [Environment]::GetEnvironmentVariable($otherName, 'Process')) {
        throw 'Mutating the caller dictionary prevented environment restoration.'
    }
    $previousWhatIf = $WhatIfPreference
    try {
        $WhatIfPreference = $true
        $result = Invoke-SuperZipProcessEnvironment -Settings @{ $name = 'required scope' } -Action {
            [Environment]::GetEnvironmentVariable($name, 'Process')
        }
        if ($result -cne 'required scope' -or [Environment]::GetEnvironmentVariable($name, 'Process') -cne $expected) {
            throw 'Required scope or restoration was skipped by the caller preference.'
        }
    } finally { $WhatIfPreference = $previousWhatIf }
    $script:InvalidActionRan = $false
    try { Invoke-SuperZipProcessEnvironment -Settings @{ 'bad=name' = 'invalid' } -Action { $script:InvalidActionRan = $true } }
    catch { if ($_.Exception.Message -notlike 'Process environment settings require*') { throw } }
    if ($script:InvalidActionRan) { throw 'Invalid environment names reached the action.' }
} finally {
    Set-SuperZipProcessEnvironmentValue -Name $name -Value $null
    Set-SuperZipProcessEnvironmentValue -Name $otherName -Value $null
}
Write-Output 'process_environment status=passed absent_empty_populated=success_and_exception nested_restore=passed'
