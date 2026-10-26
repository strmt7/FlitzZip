# Purpose: Read a bounded MSI table without installing, configuring or repairing any registered product.
# Inputs: Existing package and an allowlisted table. Outputs: String-valued row dictionaries; releases all COM owners.
function Get-MsiTable {
    param([Parameter(Mandatory = $true)][string]$Path,
        [ValidateSet('Property', 'Upgrade', 'InstallExecuteSequence', 'Registry', 'LaunchCondition', 'CustomAction', 'Binary')][string]$Table)
    $columns = @(switch ($Table) {
        'Property' { @('Property', 'Value') }
        'Upgrade' { @('UpgradeCode', 'VersionMin', 'VersionMax', 'Language', 'Attributes', 'Remove', 'ActionProperty') }
        'InstallExecuteSequence' { @('Action', 'Condition', 'Sequence') }
        'Registry' { @('Registry', 'Root', 'Key', 'Name', 'Value', 'Component_') }
        'LaunchCondition' { @('Condition', 'Description') }
        'CustomAction' { @('Action', 'Type', 'Source', 'Target') }
        'Binary' { @('Name') }
    })
    $installer = $database = $view = $record = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, [int]0))
        $query = 'SELECT ' + (($columns | ForEach-Object { '`' + $_ + '`' }) -join ',') + ' FROM `' + $Table + '`'
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($query))
        $null = $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
        for ($index = 0; $index -lt 4096; ++$index) {
            $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
            if (-not $record) { return }
            try {
                $row = @{}
                for ($column = 0; $column -lt $columns.Count; ++$column) {
                    $row[$columns[$column]] = $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @([int]($column + 1)))
                }
                Write-Output $row
            } finally { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record); $record = $null }
        }
        throw 'MSI inspection exceeded the bounded row budget.'
    } finally {
        if ($view) { $null = $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) }
        foreach ($owner in @($record, $view, $database, $installer)) {
            if ($owner) { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($owner) }
        }
    }
}

# Purpose: Verify actual compiled upgrade, rollback, feature migration and shortcut-context policy.
# Inputs: Compiled package and expected machine or internal user scope. Outputs: Metadata; rejects policy drift.
function Assert-MsiUpgradeContract {
    param([Parameter(Mandatory = $true)][string]$Path,
        [ValidateSet('perMachine', 'perUser')][string]$ExpectedScope = 'perMachine')
    $properties = @{}
    foreach ($row in @(Get-MsiTable -Path $Path -Table Property)) { $properties[$row.Property] = $row.Value }
    $allUsers = if ($ExpectedScope -eq 'perMachine') { '1' } else { '' }
    if ($properties.UpgradeCode -ine '{8E0D80F6-859D-4FE9-B082-F0D8048A8B57}' -or
        [string]$properties.ALLUSERS -cne $allUsers) {
        throw 'Installer lineage or published/internal installation scope changed.'
    }
    $sequence = @{}
    foreach ($row in @(Get-MsiTable -Path $Path -Table InstallExecuteSequence)) { $sequence[$row.Action] = [int]$row.Sequence }
    if (-not $sequence.ContainsKey('RemoveExistingProducts') -or
        $sequence.RemoveExistingProducts -le $sequence.InstallInitialize -or
        $sequence.RemoveExistingProducts -ge $sequence.ProcessComponents -or
        $sequence.MigrateFeatureStates -le $sequence.FindRelatedProducts) {
        throw 'Major upgrades must migrate features and remove old products inside the rollback transaction.'
    }
    $upgrade = @(Get-MsiTable -Path $Path -Table Upgrade)
    $remove = @($upgrade | Where-Object { ([int]$_.Attributes -band 2) -eq 0 })
    $detect = @($upgrade | Where-Object { ([int]$_.Attributes -band 2) -ne 0 })
    if ($remove.Count -ne 1 -or $detect.Count -ne 1 -or $remove[0].VersionMin -ne '' -or
        $remove[0].VersionMax -ne $properties.ProductVersion -or ([int]$remove[0].Attributes -band 513) -ne 513 -or
        $detect[0].VersionMin -ne $properties.ProductVersion -or ([int]$detect[0].Attributes -band 256) -ne 0) {
        throw 'MSI upgrade ranges must cover all older/equal versions and detect every newer version.'
    }
    $conditions = @(Get-MsiTable -Path $Path -Table LaunchCondition)
    if (-not @($conditions | Where-Object { $_.Condition.Contains($detect[0].ActionProperty) }).Count) {
        throw 'Newer-product detection has no launch condition preventing downgrade.'
    }
    $markers = @(Get-MsiTable -Path $Path -Table Registry | Where-Object {
        $_.Name -in @('desktop_shortcut', 'start_menu_entry')
    })
    if ($markers.Count -ne 2 -or @($markers | Where-Object { $_.Root -ne '-1' }).Count) {
        throw 'Optional shortcut ownership must follow the installation context (HKMU).'
    }
    return [pscustomobject]$properties
}

# Purpose: Inspect one known product/feature registration without triggering MSI consistency repair.
# Inputs: ProductCode and optional Feature name. Outputs: Native INSTALLSTATE integer; releases the COM client.
function Get-MsiRegistrationState {
    param([Parameter(Mandatory = $true)][string]$ProductCode, [string]$Feature = '')
    $installer = New-Object -ComObject WindowsInstaller.Installer
    try {
        if ($Feature) {
            return $installer.GetType().InvokeMember('FeatureState', 'GetProperty', $null, $installer, @($ProductCode, $Feature))
        }
        return $installer.GetType().InvokeMember('ProductState', 'GetProperty', $null, $installer, @($ProductCode))
    } finally { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) }
}

# Purpose: Refuse lifecycle testing alongside a real installation of this product family.
# Inputs: Stable family GUID. Outputs: Related ProductCodes only, with no installation side effects.
function Get-MsiRelatedProduct {
    param([string]$UpgradeCode = '{8E0D80F6-859D-4FE9-B082-F0D8048A8B57}')
    $installer = $products = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $products = $installer.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $installer, @($UpgradeCode))
        foreach ($product in $products) { Write-Output ([string]$product) }
    } finally {
        foreach ($owner in @($products, $installer)) {
            if ($owner -and [Runtime.InteropServices.Marshal]::IsComObject($owner)) {
                $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($owner)
            }
        }
    }
}

Export-ModuleMember -Function Get-MsiTable, Assert-MsiUpgradeContract, Get-MsiRegistrationState, Get-MsiRelatedProduct
