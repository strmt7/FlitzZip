#Requires -Version 7.0
# Purpose: Qualify compiled installer protocol, then optionally its real transactional lifecycle.
# Inputs: Default authoring mode is non-installing; Full requires an explicitly disposable elevated Windows machine.
# Outputs: Owned fixture MSIs, verbose logs and a JSON result; never executes a fixture payload as an application.
param([switch]$Full, [switch]$DisposableMachine)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
. (Join-Path $PSScriptRoot 'version.ps1')
Import-Module (Join-Path $PSScriptRoot 'msi_database.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'msi_process.psm1') -Force
if ($Full -and -not $DisposableMachine) { throw 'Real MSI lifecycle requires -DisposableMachine on an isolated VM/runner.' }
if ($Full) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Disposable installer qualification requires elevation.'
    }
    if (@(Get-MsiRelatedProduct).Count) { throw 'A related product is installed; lifecycle testing must use a clean VM.' }
}
$root = Join-Path $repo ('out/installer-lifecycle-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$cmake = Find-CMake -RepoRoot $repo
$cpack = Join-Path (Split-Path -Parent $cmake) 'cpack.exe'
$wix = Join-Path $repo 'out/tools/wix/wix.exe'
if (-not (Test-Path -LiteralPath $wix)) {
    & (Join-Path $PSScriptRoot 'install_wix.ps1')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wix)) { throw 'Pinned WiX provisioning failed.' }
}
if ((& $wix --version) -notlike '7.0.0*' -or $LASTEXITCODE -ne 0) { throw 'Installer qualification requires pinned WiX 7.0.0.' }
& $wix eula accept wix7
if ($LASTEXITCODE -ne 0) { throw 'WiX EULA acceptance failed.' }
& $wix extension add --global WixToolset.UI.wixext/7.0.0
if ($LASTEXITCODE -ne 0) { throw 'Pinned WiX UI extension is unavailable.' }
$originalPath = $env:PATH
$env:PATH = (Split-Path -Parent $wix) + [IO.Path]::PathSeparator + $env:PATH

# Purpose: Author a tiny package through the pinned production CPack/WiX path and inspect its compiled tables.
# Inputs: Version, unique fixture identity and optional obsolete/failure controls. Outputs: Package plus verified metadata.
function New-LifecyclePackage {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([string]$Version, [string]$Name, [switch]$Obsolete, [switch]$Failure,
        [ValidateSet('perMachine', 'perUser')][string]$Scope = 'perMachine')
    $null = Assert-FlitzZipMsiPackageVersion -PackageVersion $Version
    $build = Join-Path $root $Name
    if (-not $PSCmdlet.ShouldProcess($build, 'Author and inspect an installer protocol fixture')) {
        throw 'Installer qualification cannot be partially skipped.'
    }
    $productCode = [guid]::NewGuid().ToString().ToUpperInvariant()
    $arguments = @('-S', (Join-Path $repo 'tests/installer/lifecycle'), '-B', $build,
        '-G', (Find-CMakeGenerator -BuildRoot $build), '-A', 'x64', "-DFLITZZIP_ROOT=$($repo.Replace('\', '/'))",
        "-DFIXTURE_VERSION=$Version", "-DFIXTURE_ID=$Name", "-DFIXTURE_PRODUCT_CODE=$productCode",
        "-DFIXTURE_SCOPE=$Scope",
        "-DFIXTURE_OBSOLETE=$($Obsolete.IsPresent)", "-DFIXTURE_FAILURE=$($Failure.IsPresent)")
    & $cmake @arguments *> (Join-Path $root "$Name-configure.log")
    if ($LASTEXITCODE -ne 0) { throw "Fixture configure failed; see $root/$Name-configure.log" }
    if ($Failure) {
        & $cmake --build $build --config Release *> (Join-Path $root "$Name-build.log")
        if ($LASTEXITCODE -ne 0) { throw 'Rollback-control DLL build failed.' }
    }
    & $cpack -G WIX --config (Join-Path $build 'CPackConfig.cmake') -B $build *> (Join-Path $root "$Name-author.log")
    if ($LASTEXITCODE -ne 0) { throw "Fixture authoring failed; see $root/$Name-author.log" }
    $path = Join-Path $build "fixture-$Name.msi"
    $metadata = Assert-MsiUpgradeContract -Path $path -ExpectedScope $Scope
    if ($metadata.ProductVersion -ne $Version -or $metadata.ProductCode -ine "{$productCode}") {
        throw 'Compiled fixture version/ProductCode differs from its authoring inputs.'
    }
    return [pscustomobject]@{ name = $Name; path = $path; product = $metadata.ProductCode; version = $Version
        payload = (Join-Path $build 'FlitzZip.exe') }
}

# Purpose: Prove compiled policy checks reject real database drift, not just malformed authoring text.
# Inputs: One known-good MSI; only new copies inside this invocation's owned root are edited.
# Outputs: Every independent scope/range/rollback/shortcut mutation is rejected for its intended invariant.
function Test-CompiledMsiMutation {
    param([string]$Source)
    $mutations = @(
        @{ Sql = @("UPDATE ``Property`` SET ``Value``='2' WHERE ``Property``='ALLUSERS'"); Reason = 'lineage or published' },
        @{ Sql = @("DELETE FROM ``Upgrade`` WHERE ``ActionProperty``='WIX_UPGRADE_DETECTED'",
            "INSERT INTO ``Upgrade`` (``UpgradeCode``,``VersionMin``,``VersionMax``,``Language``,``Attributes``,``ActionProperty``) VALUES ('{8E0D80F6-859D-4FE9-B082-F0D8048A8B57}','0.0.1','0.1.0','1033',513,'WIX_UPGRADE_DETECTED')"); Reason = 'upgrade ranges' },
        @{ Sql = @("DELETE FROM ``Upgrade`` WHERE ``ActionProperty``='WIX_UPGRADE_DETECTED'",
            "INSERT INTO ``Upgrade`` (``UpgradeCode``,``VersionMax``,``Language``,``Attributes``,``ActionProperty``) VALUES ('{8E0D80F6-859D-4FE9-B082-F0D8048A8B57}','0.1.0','1033',1,'WIX_UPGRADE_DETECTED')"); Reason = 'upgrade ranges' },
        @{ Sql = @("UPDATE ``InstallExecuteSequence`` SET ``Sequence``=1 WHERE ``Action``='RemoveExistingProducts'"); Reason = 'rollback transaction' },
        @{ Sql = @("DELETE FROM ``InstallExecuteSequence`` WHERE ``Action``='MigrateFeatureStates'"); Reason = 'migrate features' },
        @{ Sql = @("DELETE FROM ``LaunchCondition``"); Reason = 'launch condition' },
        @{ Sql = @("UPDATE ``Registry`` SET ``Root``=1 WHERE ``Name``='desktop_shortcut'"); Reason = 'HKMU' }
    )
    for ($index = 0; $index -lt $mutations.Count; ++$index) {
        # Reflection requires a native BSTR, not the extended string returned by pipeline commands.
        [string]$path = Join-Path $root "mutation-$index.msi"
        Copy-Item -LiteralPath $Source -Destination $path
        $installer = $database = $view = $null
        try {
            $installer = New-Object -ComObject WindowsInstaller.Installer
            $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($path, [int]1))
            foreach ($query in $mutations[$index].Sql) {
                $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @([string]$query))
                $null = $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
                $null = $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
                $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
                $view = $null
            }
            $null = $database.GetType().InvokeMember('Commit', 'InvokeMethod', $null, $database, $null)
        } finally {
            if ($view) { $null = $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) }
            foreach ($owner in @($view, $database, $installer)) {
                if ($owner) { $null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($owner) }
            }
        }
        $rejected = $false
        try { $null = Assert-MsiUpgradeContract -Path $path }
        catch { $rejected = $_.Exception.Message.Contains($mutations[$index].Reason) }
        if (-not $rejected) { throw "Compiled MSI mutation $index did not fail its intended contract." }
    }
}

# Purpose: Run a known MSI phase and distinguish deliberately tested rejection from unrelated installer failure.
# Inputs: Known fixture, verb and optional expected error marker. Outputs: Success or exact proven control rejection.
function Invoke-LifecycleMsi {
    param($Package, [string]$Phase, [string]$Verb = '/i', [string[]]$Extra = @(), [string]$RejectMarker = '')
    $log = Join-Path $root "$Phase.log"
    $arguments = @($Verb, $Package.path, '/qn', '/norestart', '/L*v', $log) + $Extra
    try { Invoke-CheckedMsiExec -Phase $Phase -Arguments $arguments -LogPath $log }
    catch {
        if (-not $RejectMarker -or $_.Exception.Message -notmatch 'failed with exit code 1603\.' -or
            -not (Get-Content -LiteralPath $log -Raw).Contains($RejectMarker)) { throw }
        return
    }
    if ($RejectMarker) { throw "$Phase unexpectedly accepted an operation that must be rejected." }
}

# Purpose: Verify complete package ownership and preserved user data after each lifecycle phase.
# Inputs: Current expected fixture and every other known fixture ProductCode. Outputs: Exact payload and registration assertions.
function Assert-LifecycleState {
    param($Current, [object[]]$Packages)
    $expected = if ($Current) { @($Current.product) } else { @() }
    $related = @(Get-MsiRelatedProduct)
    if (@(Compare-Object -ReferenceObject @($expected + '__sentinel__') -DifferenceObject @($related + '__sentinel__')).Count) {
        throw 'A lifecycle phase left an unexpected related product registration.'
    }
    foreach ($package in $Packages) {
        $state = Get-MsiRegistrationState -ProductCode $package.product
        if (($package.product -eq $Current.product -and $state -ne 5) -or
            ($package.product -ne $Current.product -and $state -ne -1)) { throw 'Product registration state is inconsistent.' }
    }
    $payload = Join-Path $install 'bin/FlitzZip.exe'
    if ($Current) {
        if ((Get-FileHash -LiteralPath $payload).Hash -ne (Get-FileHash -LiteralPath $Current.payload).Hash) {
            throw 'Installed payload differs from the intended package bytes.'
        }
        if ((Get-MsiRegistrationState -ProductCode $Current.product -Feature DesktopShortcutFeature) -ne 2 -or
            (Get-MsiRegistrationState -ProductCode $Current.product -Feature StartMenuShortcutFeature) -ne 3) {
            throw 'Major upgrade did not preserve optional feature choices.'
        }
        $obsolete = Join-Path $install 'bin/obsolete-owned.txt'
        if ($Current.name -eq 'old' -and
            (-not (Test-Path -LiteralPath $obsolete) -or [IO.File]::ReadAllText($obsolete) -cne "old package owned this file`n")) {
            throw 'Old-version fixture did not install the independently expected obsolete payload.'
        }
        if ($Current.name -ne 'old' -and (Test-Path -LiteralPath $obsolete)) {
            throw 'Upgrade left an obsolete package-owned file behind.'
        }
    } elseif (Test-Path -LiteralPath $payload) { throw 'Uninstall left a package-owned payload behind.' }
    if ((Test-Path -LiteralPath $desktopShortcut) -or
        ([bool](Test-Path -LiteralPath $menuShortcut) -ne [bool]$Current)) {
        throw 'Installer shortcut files disagree with the retained feature choices.'
    }
    foreach ($path in $sentinels.Keys) {
        if ((Get-FileHash -LiteralPath $path).Hash -ne $sentinels[$path]) { throw 'Installer changed independent user data.' }
    }
}

# Purpose: Exercise direct, sequential, rejected, failed, repair and replacement paths on an explicitly disposable machine.
# Inputs: Authoring fixtures and owned test root. Outputs: All expected lifecycle states, preserving user-data sentinels.
function Test-InstalledLifecycle {
    param([object[]]$Packages)
    $old, $middle, $new, $replacement, $failed = $Packages
    $script:install = Join-Path $root 'installed'
    $script:desktopShortcut = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'FlitzZip.lnk'
    $script:menuShortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'FlitzZip/FlitzZip.lnk'
    if ((Test-Path -LiteralPath $desktopShortcut) -or (Test-Path -LiteralPath $menuShortcut)) {
        throw 'Disposable runner already has product shortcuts; refusing to alter them.'
    }
    $profileDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'FlitzZip'
    if (Test-Path -LiteralPath $profileDirectory) { throw 'Disposable runner already has a user profile; refusing to alter it.' }
    $null = New-Item -ItemType Directory -Path $profileDirectory
    $script:sentinels = @{}
    foreach ($name in @('settings.json', 'settings-v4.json', 'settings-v999.json', 'settings-v4.json.bak', 'user-archive.flzip')) {
        $path = Join-Path $profileDirectory $name
        [IO.File]::WriteAllText($path, "independent opaque user data: $name")
        $script:sentinels[$path] = (Get-FileHash -LiteralPath $path).Hash
    }
    $choices = @("INSTALL_ROOT=$install", 'ADDLOCAL=ProductFeature,StartMenuShortcutFeature', 'REMOVE=DesktopShortcutFeature')
    try {
        Invoke-LifecycleMsi -Package $old -Phase fresh-old -Extra $choices
        Assert-LifecycleState -Current $old -Packages $Packages
        Invoke-LifecycleMsi -Package $new -Phase direct-skipped-upgrade
        Assert-LifecycleState -Current $new -Packages $Packages
        Invoke-LifecycleMsi -Package $old -Phase reject-downgrade -RejectMarker 'A later version of'
        Assert-LifecycleState -Current $new -Packages $Packages
        Invoke-LifecycleMsi -Package $failed -Phase rollback-failed-upgrade -RejectMarker 'CustomAction FixtureRejectUpgrade returned actual error code 1603'
        Assert-LifecycleState -Current $new -Packages $Packages
        Invoke-LifecycleMsi -Package $replacement -Phase authorized-same-version-replacement
        Assert-LifecycleState -Current $replacement -Packages $Packages
        Remove-Item -LiteralPath (Join-Path $install 'bin/FlitzZip.exe')
        Invoke-LifecycleMsi -Package $replacement -Phase repair-missing-payload -Verb '/fa'
        Assert-LifecycleState -Current $replacement -Packages $Packages
        Invoke-LifecycleMsi -Package $replacement -Phase uninstall-direct -Verb '/x'
        Assert-LifecycleState -Current $null -Packages $Packages
        Invoke-LifecycleMsi -Package $old -Phase reinstall-old -Extra $choices
        Invoke-LifecycleMsi -Package $middle -Phase sequential-middle
        Assert-LifecycleState -Current $middle -Packages $Packages
        Invoke-LifecycleMsi -Package $new -Phase sequential-new
        Assert-LifecycleState -Current $new -Packages $Packages
        Invoke-LifecycleMsi -Package $new -Phase uninstall-sequential -Verb '/x'
        Assert-LifecycleState -Current $null -Packages $Packages
    } finally {
        foreach ($package in $Packages) {
            if ((Get-MsiRegistrationState -ProductCode $package.product) -eq 5) {
                Invoke-LifecycleMsi -Package $package -Phase ("cleanup-" + $package.name) -Verb '/x'
            }
        }
        # This directory was absent and was created only by this explicit disposable-machine test.
        $expectedProfile = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'FlitzZip'))
        if ([IO.Path]::GetFullPath($profileDirectory) -cne $expectedProfile -or
            ((Get-Item -LiteralPath $profileDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing cleanup of a redirected or unexpected test profile.'
        }
        Remove-Item -LiteralPath $profileDirectory -Recurse -Force
    }
}

try {
    $packages = @(
        (New-LifecyclePackage -Version 0.1.0 -Name old -Obsolete),
        (New-LifecyclePackage -Version 0.2.0 -Name middle),
        (New-LifecyclePackage -Version 31.4.128 -Name new),
        (New-LifecyclePackage -Version 31.4.128 -Name replacement),
        (New-LifecyclePackage -Version 31.4.129 -Name failed -Failure)
    )
    Test-CompiledMsiMutation -Source $packages[0].path
    $perUser = New-LifecyclePackage -Version 0.1.0 -Name internal-per-user -Scope perUser
    $rejectedScope = $false
    try { $null = Assert-MsiUpgradeContract -Path $perUser.path -ExpectedScope perMachine }
    catch { $rejectedScope = $_.Exception.Message.Contains('installation scope') }
    if (-not $rejectedScope) { throw 'Published package admission accepted the internal per-user scope.' }
    if ($Full) { Test-InstalledLifecycle -Packages $packages }
    $result = @{ schema = 'flitzzip.installer-lifecycle.v1'; mode = $(if ($Full) { 'installed' } else { 'authoring-only' })
        passed = $true; sourceCommit = (& git -C $repo rev-parse HEAD)
        internalUserScopeVerified = $perUser.product
        packages = @($packages | Select-Object name, product, version,
            @{ Name = 'sha256'; Expression = { (Get-FileHash -LiteralPath $_.path -Algorithm SHA256).Hash.ToLowerInvariant() } }) }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding utf8
    Write-Output "Installer lifecycle $($result.mode) passed. Evidence: $root"
} finally { $env:PATH = $originalPath }
