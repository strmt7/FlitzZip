# Purpose: Bind GUI test gestures to independently declared persistent identifiers, not saved UI ordinals.
# Inputs: One supported control kind. Outputs: Its expected visible row order for behavioral UI assertions.
function Get-GuiSettingsChoiceValue {
    param([ValidateSet('LogLevel', 'Retention', 'Format')][string]$Kind)
    switch ($Kind) {
        'LogLevel' { return @('information', 'warning', 'debug') }
        'Retention' { return @(7, 14, 30) }
        'Format' { return @('.flzip', '.zip', '.tar.gz', '.tgz', '.tar', '.gz', '.zst', '.zstd', '.tar.zst',
                '.tzst', '.tar.bz2', '.tbz2', '.tbz', '.bz2', '.cpio', '.cpio.gz', '.cpgz', '.ar', '.Z') }
    }
}

# Purpose: Restore a smoke scenario's control choice using its semantic profile value.
# Inputs: Parsed current-generation settings and one control kind. Outputs: An exact row or an explicit error.
function Get-GuiSettingsChoiceIndex {
    param([Parameter(Mandatory = $true)]$Settings,
          [ValidateSet('LogLevel', 'Retention', 'Format')][string]$Kind)
    $value = switch ($Kind) {
        'LogLevel' { $Settings.logLevel }
        'Retention' { $Settings.logRetentionDays }
        'Format' { $Settings.compressionFormat }
    }
    $values = @(Get-GuiSettingsChoiceValue -Kind $Kind)
    for ($index = 0; $index -lt $values.Count; ++$index) {
        if ([string]$values[$index] -ceq [string]$value) { return $index }
    }
    throw "GUI settings contain an unsupported $Kind choice."
}
