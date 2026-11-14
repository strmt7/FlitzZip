$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package_runtime.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('flitzzip-runtime-contract-' + [guid]::NewGuid().ToString('N'))

# Purpose: Require the production boundary to reject one independent invalid payload/state.
# Inputs: Action executes the actual validator. Outputs: Throws if the invalid condition was admitted.
function Assert-RuntimeRejected {
    param([scriptblock]$Action, [string]$ExpectedMessage = '')
    $rejected = $false
    try { & $Action } catch {
        if ($ExpectedMessage -and $_.Exception.Message -notlike "*$ExpectedMessage*") { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Runtime contract admitted an invalid payload or capability state.' }
}

# Purpose: Exercise exact native-tool identity without executing a foreign or damaged binary.
# Inputs: Independent successful and failed probe observations. Outputs: Rejects every ambiguous identity.
function Test-WixVersionAdmission {
    $tool = Get-FlitzZipWixToolchain -RepoRoot $fixture
    Assert-FlitzZipWixVersion -Output @('7.0.1+642f1a0') -ExitCode 0 -Expected $tool.CliVersion
    foreach ($output in @(@('7.0.10'), @('7.0.1-rc.1'), @('7.0.0+642f1a0'), @('7.0.1'),
            @('7.0.1+642f1a0', 'extra'), @(), @('7.0.1+unexpected'))) {
        Assert-RuntimeRejected -ExpectedMessage 'version admission' -Action {
            Assert-FlitzZipWixVersion -Output $output -ExitCode 0 -Expected $tool.CliVersion
        }
    }
    Assert-RuntimeRejected -ExpectedMessage 'version admission' -Action {
        Assert-FlitzZipWixVersion -Output @('7.0.1+642f1a0') -ExitCode 17 -Expected $tool.CliVersion
    }
    Assert-RuntimeRejected { Resolve-FlitzZipWix -RepoRoot $fixture }
}

# Purpose: Reject a missing, invalid or redirected UI assembly before CPack can load it.
# Inputs: Private cache fixtures contain no executable extension. Outputs: Every malformed cache remains rejected.
function Test-WixUiAdmission {
    $tool = Get-FlitzZipWixToolchain -RepoRoot $fixture
    Assert-RuntimeRejected { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    $directory = Join-Path $tool.ExtensionHome ".wix/extensions/WixToolset.UI.wixext/$($tool.Version)/wixext7"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $assembly = Join-Path $directory 'WixToolset.UI.wixext.dll'
    [IO.File]::WriteAllText($assembly, 'invalid extension fixture')
    Assert-RuntimeRejected -ExpectedMessage 'extension identity' -Action { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    Remove-Item -LiteralPath $assembly
    $real = Join-Path $fixture 'real-extension-cache'
    New-Item -ItemType Directory -Path $real | Out-Null
    $link = Join-Path $directory 'WixToolset.UI.wixext.dll'
    New-Item -ItemType Junction -Path $link -Target $real | Out-Null
    try {
        Assert-RuntimeRejected -ExpectedMessage 'redirected' -Action { Get-FlitzZipWixUiArgument -RepoRoot $fixture }
    } finally { [IO.Directory]::Delete($link) }
}

# Purpose: Prove packaging restores the caller's discovery state after both normal completion and failure.
# Inputs: A controlled resolver owns no external installation. Outputs: Exact original environment survives both outcomes.
function Test-WixEnvironmentOwnership {
    $savedResolver = ${function:Resolve-FlitzZipWix}
    $before = @($env:PATH, $env:WIX, $env:WIX_EXTENSIONS)
    try {
        function Resolve-FlitzZipWix { param([string]$RepoRoot) return (Get-FlitzZipWixToolchain -RepoRoot $RepoRoot).Path }
        foreach ($fail in @($false, $true)) {
            $packagingAction = {
                param($wix, $extension)
                $expected = Get-FlitzZipWixToolchain -RepoRoot $fixture
                if ($wix -cne $expected.Path -or $extension -cne $expected.UiExtension -or
                    $env:WIX_EXTENSIONS -cne $expected.ExtensionHome -or $env:WIX -cne (Split-Path -Parent $wix) -or
                    -not $env:PATH.StartsWith($env:WIX + [IO.Path]::PathSeparator, [StringComparison]::Ordinal)) {
                    throw 'Packaging did not receive its owned tool environment.'
                }
                if ($fail) { throw 'Independent packaging failure.' }
            }
            if ($fail) {
                Assert-RuntimeRejected -ExpectedMessage 'Independent packaging failure' -Action {
                    Invoke-FlitzZipWixEnvironment -RepoRoot $fixture -Action $packagingAction
                }
            } else { Invoke-FlitzZipWixEnvironment -RepoRoot $fixture -Action $packagingAction }
            if ($env:PATH -cne $before[0] -or $env:WIX -cne $before[1] -or $env:WIX_EXTENSIONS -cne $before[2]) {
                throw 'Packaging changed its caller environment.'
            }
        }
    } finally { ${function:Resolve-FlitzZipWix} = $savedResolver }
}

# Exercise the real process exit boundary and artifact owner together. The child
# is a deterministic external generator; it does not install a protocol fixture.
function Test-PackageGenerationTransaction {
    param([string]$Root)
    $output = Join-Path $Root 'packages'
    New-Item -ItemType Directory -Path $output | Out-Null
    $child = Join-Path $Root 'generator.ps1'
    @'
param([string]$Output, [string]$Behavior)
$ErrorActionPreference = 'Stop'
if ($Behavior -eq 'missing') { exit 0 }
if ($Behavior -eq 'empty') { [IO.File]::WriteAllBytes($Output, [byte[]]@()); exit 0 }
[IO.File]::WriteAllText($Output, 'new complete package')
if ($Behavior -eq 'failed') { exit 17 }
exit 0
'@ | Set-Content -LiteralPath $child
    $shellName = if (Test-Path -LiteralPath (Join-Path $PSHOME 'powershell.exe')) { 'powershell.exe' } else { 'pwsh.exe' }
    $shell = Join-Path $PSHOME $shellName
    $destination = Join-Path $output 'candidate.msi'
    [IO.File]::WriteAllText($destination, 'preceding package')
    foreach ($behavior in @('failed', 'missing', 'empty', 'invalid')) {
        $generate = {
            param($candidate)
            Invoke-FlitzZipPackageTool -FilePath $shell -Operation 'Fixture generator' -Arguments @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $child, '-Output', $candidate, '-Behavior', $behavior)
        }
        $validate = {
            param($candidate)
            if ($behavior -eq 'invalid') { throw 'Independent artifact validation rejected the candidate.' }
            if ([IO.File]::ReadAllText($candidate) -cne 'new complete package') { throw 'Unexpected generated bytes.' }
        }
        $expected = switch ($behavior) {
            'failed' { 'exit code 17' }
            'empty' { 'complete regular artifact' }
            'invalid' { 'validation rejected' }
            default { '' }
        }
        Assert-RuntimeRejected -ExpectedMessage $expected -Action {
            Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
                -Generate $generate -Validate $validate
        }
        if ([IO.File]::ReadAllText($destination) -cne 'preceding package') {
            throw "Failed $behavior generation replaced the preceding artifact."
        }
        if (@(Get-ChildItem -LiteralPath $output -Directory).Count -ne 0) { throw 'Failed generation leaked its workspace.' }
    }
    $behavior = 'success'
    $published = Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
        -Generate $generate -Validate $validate
    if ($published -cne $destination -or [IO.File]::ReadAllText($published) -cne 'new complete package') {
        throw 'Successful generation did not replace the exact preceding artifact.'
    }
    Remove-Item -LiteralPath $destination
    $published = Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.msi' `
        -Generate $generate -Validate $validate
    if ([IO.File]::ReadAllText($published) -cne 'new complete package') { throw 'First publication failed.' }
    foreach ($name in @('../escaped.msi', 'nested/file.msi', 'C:\escaped.msi', '.')) {
        Assert-RuntimeRejected {
            Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName $name `
                -Generate { throw 'Invalid name reached generation.' }
        }
    }
    if (@(Get-ChildItem -LiteralPath $output -Directory).Count -ne 0) { throw 'Successful generation leaked its workspace.' }
    $occupied = Join-Path $output 'occupied.msi'
    New-Item -ItemType Directory -Path $occupied | Out-Null
    Assert-RuntimeRejected {
        Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'occupied.msi' `
            -Generate $generate -Validate $validate
    }
    if (-not (Test-Path -LiteralPath $occupied -PathType Container)) { throw 'Publication changed an unrelated directory.' }
}

# Build a declared installation with binary, empty and hidden Unicode files.
# A duplicate manifest line represents a legal repeated CMake install event.
function New-PortablePayloadFixture {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([string]$Root, [string]$Name)
    $work = Join-Path $Root $Name
    if (-not $PSCmdlet.ShouldProcess($work, 'Create isolated installed-payload fixture')) { return }
    $stage = Join-Path $work 'stage'
    New-Item -ItemType Directory -Path (Join-Path $stage 'bin'), (Join-Path $stage 'docs') -Force | Out-Null
    $application = Join-Path $stage 'bin/app.exe'
    $documentName = 'docs/' + [char]0x65E5 + '.txt'
    $document = Join-Path $stage $documentName
    $empty = Join-Path $stage 'empty.dat'
    [IO.File]::WriteAllBytes($application, [byte[]]@(0, 255, 1, 10))
    [IO.File]::WriteAllText($document, 'independent document', [Text.UTF8Encoding]::new($false))
    [IO.File]::SetAttributes($document, [IO.FileAttributes]::Hidden)
    [IO.File]::WriteAllBytes($empty, [byte[]]@())
    $manifest = Join-Path $work 'install_manifest.txt'
    [IO.File]::WriteAllLines($manifest, [string[]]@($application, $document, $empty, $application),
        [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ work = $work; stage = $stage; manifest = $manifest; application = $application
        documentName = $documentName }
}

# Independent writer for valid and faulty candidate archives. Its caller selects
# every entry and byte; it does not use the production portable producer.
function New-PortableFixtureArchive {
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
    param([string]$Path, [object[]]$Entries)
    if (-not $PSCmdlet.ShouldProcess($Path, 'Create independent ZIP fixture')) { return }
    Add-Type -AssemblyName System.IO.Compression
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $archive = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($entry in $Entries) {
                $stream = $archive.CreateEntry($entry.name).Open()
                try { $stream.Write($entry.bytes, 0, $entry.bytes.Length) }
                finally { $stream.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $file.Dispose() }
}

# Challenge both the declared-file producer and independent validator with actual
# bytes. Undeclared files are not payload; declared hidden/Unicode/empty files are.
function Test-PortablePayloadContent {
    param([string]$Root)
    $fixtureData = New-PortablePayloadFixture -Root $Root -Name 'portable-contents' -Confirm:$false
    $payload = Get-FlitzZipInstalledPayload -StageRoot $fixtureData.stage -InstallManifest $fixtureData.manifest
    if ($payload.get_Count() -ne 3) { throw 'Repeated install events changed the installed-file set.' }
    [IO.File]::WriteAllText((Join-Path $fixtureData.stage 'builder-diagnostic.log'), 'not an installed file')
    $preview = Join-Path $fixtureData.work 'preview.zip'
    New-FlitzZipPortablePackage -Path $preview -Payload $payload -WhatIf -Confirm:$false
    if (Test-Path -LiteralPath $preview) { throw 'Portable WhatIf preview created an artifact.' }
    $path = Join-Path $fixtureData.work 'candidate.zip'
    New-FlitzZipPortablePackage -Path $path -Payload $payload
    Assert-FlitzZipPortablePayload -Path $path -Payload $payload
    $archive = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        $expectedNames = @('bin/app.exe', $fixtureData.documentName, 'empty.dat')
        if (@(Compare-Object -ReferenceObject $expectedNames -DifferenceObject @($archive.Entries.FullName) -CaseSensitive).Count -ne 0 -or
            $null -ne $archive.GetEntry('builder-diagnostic.log') -or $null -eq $archive.GetEntry('empty.dat') -or
            $archive.GetEntry('empty.dat').Length -ne 0) { throw 'Portable installed-file inventory differs.' }
        $stream = $archive.GetEntry('bin/app.exe').Open()
        $bytes = [IO.MemoryStream]::new()
        try {
            $stream.CopyTo($bytes)
            if ([BitConverter]::ToString($bytes.ToArray()) -cne '00-FF-01-0A') { throw 'Portable binary bytes differ.' }
        } finally { $bytes.Dispose(); $stream.Dispose() }
        $stream = $archive.GetEntry($fixtureData.documentName).Open()
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
        try { if ($reader.ReadToEnd() -cne 'independent document') { throw 'Hidden Unicode document was lost.' } }
        finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
    [IO.File]::WriteAllBytes($fixtureData.application, [byte[]]@(0, 255, 1, 11))
    $changed = Join-Path $fixtureData.work 'changed.zip'
    New-FlitzZipPortablePackage -Path $changed -Payload $payload
    Assert-RuntimeRejected -ExpectedMessage 'ZIP installed file bytes differ*' {
        Assert-FlitzZipPortablePayload -Path $changed -Payload $payload
    }
}

# Wrong inventory and equal-length corruption must fail independently of the
# production writer. Renaming rejected candidates verifies reader-handle release.
function Test-PortablePayloadRejection {
    param([string]$Root)
    $fixtureData = New-PortablePayloadFixture -Root $Root -Name 'portable-rejection' -Confirm:$false
    $payload = Get-FlitzZipInstalledPayload -StageRoot $fixtureData.stage -InstallManifest $fixtureData.manifest
    foreach ($case in @('missing', 'extra', 'duplicate', 'changed', 'length')) {
        $entries = @(
            [pscustomobject]@{ name = 'bin/app.exe'; bytes = [byte[]]@(0, 255, 1, 10) },
            [pscustomobject]@{ name = $fixtureData.documentName; bytes = [Text.Encoding]::UTF8.GetBytes('independent document') },
            [pscustomobject]@{ name = 'empty.dat'; bytes = [byte[]]@() }
        )
        switch ($case) {
            'missing' { $entries = $entries[0..1] }
            'extra' { $entries += [pscustomobject]@{ name = 'uninstalled.log'; bytes = [byte[]]@(1) } }
            'duplicate' { $entries += [pscustomobject]@{ name = 'BIN/APP.EXE'; bytes = [byte[]]@(0, 255, 1, 10) } }
            'changed' { $entries[0].bytes = [byte[]]@(0, 255, 1, 11) }
            'length' { $entries[0].bytes = [byte[]]@(1) }
        }
        $path = Join-Path $fixtureData.work "$case.zip"
        New-PortableFixtureArchive -Path $path -Entries $entries -Confirm:$false
        Assert-RuntimeRejected { Assert-FlitzZipPortablePayload -Path $path -Payload $payload }
        [IO.File]::Move($path, "$path.closed")
    }
    $valid = Join-Path $fixtureData.work 'recovered.zip'
    New-FlitzZipPortablePackage -Path $valid -Payload $payload
    Assert-FlitzZipPortablePayload -Path $valid -Payload $payload
}

# An unreviewed wildcard producer still cannot publish extra stage files. Keep
# the preceding artifact intact, then prove recovery with the canonical producer.
function Test-PortablePublicationRecovery {
    param([string]$Root)
    $fixtureData = New-PortablePayloadFixture -Root $Root -Name 'portable-publication' -Confirm:$false
    $payload = Get-FlitzZipInstalledPayload -StageRoot $fixtureData.stage -InstallManifest $fixtureData.manifest
    [IO.File]::WriteAllText((Join-Path $fixtureData.stage 'unexpected-diagnostic.txt'), 'uninstalled')
    $output = Join-Path $fixtureData.work 'output'
    New-Item -ItemType Directory -Path $output | Out-Null
    $destination = Join-Path $output 'candidate.zip'
    [IO.File]::WriteAllText($destination, 'preceding artifact')
    Assert-RuntimeRejected -ExpectedMessage '*outside the install manifest*' {
        Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.zip' -Generate {
            param($candidate)
            [IO.Compression.ZipFile]::CreateFromDirectory($fixtureData.stage, $candidate)
        } -Validate {
            param($candidate)
            Assert-FlitzZipPortablePayload -Path $candidate -Payload $payload
        }
    }
    if ([IO.File]::ReadAllText($destination) -cne 'preceding artifact' -or
        @(Get-ChildItem -LiteralPath $output -Directory).Count -ne 0) {
        throw 'Rejected portable generation replaced the preceding artifact or leaked its workspace.'
    }
    $published = Publish-FlitzZipGeneratedPackage -OutputDirectory $output -FileName 'candidate.zip' -Generate {
        param($candidate)
        New-FlitzZipPortablePackage -Path $candidate -Payload $payload
    } -Validate {
        param($candidate)
        Assert-FlitzZipPortablePayload -Path $candidate -Payload $payload
    }
    Assert-FlitzZipPortablePayload -Path $published -Payload $payload
}

# Missing/invalid declarations cannot silently turn the whole stage into payload.
# A valid snapshot remains usable after each rejected manifest observation.
function Test-PortableManifestAdmission {
    param([string]$Root)
    $fixtureData = New-PortablePayloadFixture -Root $Root -Name 'portable-manifest' -Confirm:$false
    $outside = Join-Path $fixtureData.work 'unrelated.txt'
    [IO.File]::WriteAllText($outside, 'preserve caller data')
    $cases = @(
        [pscustomobject]@{ lines = [string[]]@() },
        [pscustomobject]@{ lines = [string[]]@('bin/app.exe') },
        [pscustomobject]@{ lines = [string[]]@($outside) },
        [pscustomobject]@{ lines = [string[]]@((Join-Path $fixtureData.stage 'absent.bin')) }
    )
    foreach ($case in $cases) {
        $manifest = Join-Path $fixtureData.work ('bad-' + [guid]::NewGuid().ToString('N') + '.txt')
        [IO.File]::WriteAllLines($manifest, $case.lines, [Text.UTF8Encoding]::new($false))
        Assert-RuntimeRejected { Get-FlitzZipInstalledPayload -StageRoot $fixtureData.stage -InstallManifest $manifest }
    }
    if ([IO.File]::ReadAllText($outside) -cne 'preserve caller data') { throw 'Manifest admission changed caller data.' }
    $payload = Get-FlitzZipInstalledPayload -StageRoot $fixtureData.stage -InstallManifest $fixtureData.manifest
    if ($payload.get_Count() -ne 3) { throw 'Rejected manifests prevented valid snapshot recovery.' }
    $trailingRoot = $fixtureData.stage + [IO.Path]::DirectorySeparatorChar
    if ((Get-FlitzZipInstalledPayload -StageRoot $trailingRoot -InstallManifest $fixtureData.manifest).Count -ne 3) {
        throw 'A trailing root separator changed the installed-file snapshot.'
    }
}

# Legal filenames can collide with PowerShell's dictionary property adaptation.
# They must remain ordinary payload members in snapshots, ZIPs and installed checks.
function Test-PayloadMemberNameIdentity {
    param([string]$Root)
    $work = Join-Path $Root 'member-names'
    $stage = Join-Path $work 'stage'
    $installed = Join-Path $work 'installed'
    New-Item -ItemType Directory -Path $stage, $installed -Force | Out-Null
    $names = @('Count', 'Keys', 'Values', 'Add', 'ContainsKey', 'GetEnumerator', 'get_Count', 'get_Keys', 'psbase')
    $files = [Collections.Generic.List[string]]::new()
    foreach ($name in $names) {
        $source = Join-Path $stage $name
        $bytes = [Text.Encoding]::UTF8.GetBytes("independent file $name")
        [IO.File]::WriteAllBytes($source, $bytes)
        [IO.File]::WriteAllBytes((Join-Path $installed $name), $bytes)
        $files.Add($source)
    }
    $manifest = Join-Path $work 'install_manifest.txt'
    [IO.File]::WriteAllLines($manifest, $files.ToArray(), [Text.UTF8Encoding]::new($false))
    $payload = Get-FlitzZipInstalledPayload -StageRoot $stage -InstallManifest $manifest
    if ($payload.get_Count() -ne $names.Length) { throw 'Dictionary member names changed the payload set.' }
    $archive = Join-Path $work 'candidate.zip'
    New-FlitzZipPortablePackage -Path $archive -Payload $payload -Confirm:$false
    Assert-FlitzZipPortablePayload -Path $archive -Payload $payload
    $reference = Get-FlitzZipPortablePayload -Path $archive
    if (@(Compare-Object -ReferenceObject $names -DifferenceObject @($reference.get_Keys()) -CaseSensitive).Count) {
        throw 'A dictionary member name was lost from the actual ZIP.'
    }
    Assert-FlitzZipInstalledPayload -PackageRoot $installed -Payload $reference
    [IO.File]::WriteAllText((Join-Path $installed 'Count'), 'damaged file')
    Assert-RuntimeRejected -Action { Assert-FlitzZipInstalledPayload -PackageRoot $installed -Payload $reference }
}

# Require every declared installed file, including non-executable Unicode content,
# to match the captured build bytes. Files outside installer ownership survive.
function Test-InstalledPayloadParity {
    param([string]$Root)
    $source = New-PortablePayloadFixture -Root $Root -Name 'parity-source' -Confirm:$false
    $installed = New-PortablePayloadFixture -Root $Root -Name 'parity-installed' -Confirm:$false
    $archive = Join-Path $source.work 'distribution.zip'
    New-PortableFixtureArchive -Path $archive -Entries @(
        @{ name = 'bin/app.exe'; bytes = [byte[]]@(0, 255, 1, 10) },
        @{ name = $source.documentName; bytes = [Text.Encoding]::UTF8.GetBytes('independent document') },
        @{ name = 'empty.dat'; bytes = [byte[]]@() }) -Confirm:$false
    $payload = Get-FlitzZipPortablePayload -Path $archive
    # A later CPack install cannot redefine the comparison cohort.
    [IO.File]::WriteAllText($source.manifest, 'different CPack staging tree')
    $unmanaged = Join-Path $installed.stage 'user-owned.txt'
    [IO.File]::WriteAllText($unmanaged, 'retained user data')
    Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload
    foreach ($name in @('bin/app.exe', $installed.documentName, 'empty.dat')) {
        $path = Join-Path $installed.stage $name
        $original = [IO.File]::ReadAllBytes($path)
        Remove-Item -LiteralPath $path -Force
        Assert-RuntimeRejected -Action { Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload }
        [IO.File]::WriteAllBytes($path, $original)
        if ($original.Length -gt 0) {
            $changed = [byte[]]$original.Clone()
            $changed[0] = $changed[0] -bxor 1
            [IO.File]::WriteAllBytes($path, $changed)
            Assert-RuntimeRejected -ExpectedMessage 'Installed payload differs' -Action {
                Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload
            }
            # Failed hashing must release the target handle before recovery.
            $moved = $path + '.damaged'
            Move-Item -LiteralPath $path -Destination $moved
            Remove-Item -LiteralPath $moved -Force
            [IO.File]::WriteAllBytes($path, [byte[]]@())
            Assert-RuntimeRejected -ExpectedMessage 'Installed payload differs' -Action {
                Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload
            }
            [IO.File]::WriteAllBytes($path, $original)
        }
        Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload
    }
    foreach ($name in $payload.get_Keys()) {
        Remove-Item -LiteralPath (Join-Path $installed.stage $name) -Force
    }
    Assert-RuntimeRejected -Action { Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload }
    foreach ($name in $payload.get_Keys()) {
        Copy-Item -LiteralPath (Join-Path $source.stage $name) -Destination (Join-Path $installed.stage $name) -Force
    }
    Assert-FlitzZipInstalledPayload -PackageRoot $installed.stage -Payload $payload
    if ([IO.File]::ReadAllText($unmanaged) -cne 'retained user data' -or
        [BitConverter]::ToString([IO.File]::ReadAllBytes($source.application)) -cne '00-FF-01-0A') {
        throw 'Installed parity inspection modified source bytes or unmanaged data.'
    }
}

# The ZIP is written independently and differs from any installation stage.
# Assert what the consumer receives, callback failure cleanup and later reuse.
function Test-PortableSmokeLifecycle {
    param([string]$Root)
    $work = Join-Path $Root ('portable smoke ' + [char]0x65E5)
    New-Item -ItemType Directory -Path $work | Out-Null
    $archive = Join-Path $work 'actual.zip'
    New-PortableFixtureArchive -Path $archive -Entries @(
        @{ name = 'bin/app.exe'; bytes = [byte[]]@(0, 255, 1, 10) },
        @{ name = 'empty.dat'; bytes = [byte[]]@() }) -Confirm:$false
    $digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $unrelated = Join-Path $work 'keep.txt'
    [IO.File]::WriteAllText($unrelated, 'unrelated work')
    $state = [pscustomobject]@{ calls = 0; workspace = '' }
    $consume = {
        param($packageRoot, $workspace)
        ++$state.calls
        $state.workspace = $workspace
        if ($packageRoot -ne (Join-Path $workspace 'package') -or
            [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $packageRoot 'bin/app.exe'))) -cne 'AP8BCg==' -or
            (Get-Item -LiteralPath (Join-Path $packageRoot 'empty.dat')).Length -ne 0) {
            throw 'Portable smoke did not consume the independently written ZIP bytes.'
        }
        return 'consumer completed'
    }
    $result = Invoke-FlitzZipPortableSmoke -ArchivePath $archive -WorkRoot $work -Test $consume -Confirm:$false
    if ($result -cne 'consumer completed' -or $state.calls -ne 1 -or
        (Test-Path -LiteralPath $state.workspace)) {
        throw 'Portable smoke lost its result, repeated the callback or retained extraction files.'
    }
    $failure = {
        param($packageRoot, $workspace)
        $null = $packageRoot
        $state.workspace = $workspace
        throw 'independent portable consumer failure'
    }
    Assert-RuntimeRejected -ExpectedMessage 'independent portable consumer failure' -Action {
        Invoke-FlitzZipPortableSmoke -ArchivePath $archive -WorkRoot $work -Test $failure -Confirm:$false
    }
    if (Test-Path -LiteralPath $state.workspace) { throw 'Failed consumer retained its extraction directory.' }
    $null = Invoke-FlitzZipPortableSmoke -ArchivePath $archive -WorkRoot $work -Test $consume -Confirm:$false
    $null = Invoke-FlitzZipPortableSmoke -ArchivePath $archive -WorkRoot $work -Test $consume -WhatIf
    if ($state.calls -ne 2 -or (Test-Path -LiteralPath $state.workspace) -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne $digest -or
        [IO.File]::ReadAllText($unrelated) -cne 'unrelated work') {
        throw 'Portable preview, recovery or cleanup changed its inputs or unrelated work.'
    }
    $damaged = Join-Path $work 'incomplete.zip'
    [IO.File]::WriteAllBytes($damaged, [byte[]]@(80, 75, 3, 4))
    Assert-RuntimeRejected -Action {
        Invoke-FlitzZipPortableSmoke -ArchivePath $damaged -WorkRoot $work -Test $consume -Confirm:$false
    }
    if ($state.calls -ne 2 -or @(Get-ChildItem -LiteralPath $work -Directory).Count -ne 0) {
        throw 'Failed ZIP extraction invoked the consumer or retained an owned directory.'
    }
    $null = Invoke-FlitzZipPortableSmoke -ArchivePath $archive -WorkRoot $work -Test $consume -Confirm:$false
    if ($state.calls -ne 3 -or (Test-Path -LiteralPath $state.workspace)) {
        throw 'Portable extraction could not recover after a failed archive.'
    }
}

try {
    New-Item -ItemType Directory -Path (Join-Path $fixture 'bin') -Force | Out-Null
    $entries = @()
    foreach ($name in @('libzstd.dll', 'libwim-15.dll', 'flitzzip_hip_kernels.dll')) {
        $file = Join-Path $fixture "bin/$name"
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::UTF8.GetBytes("fixture $name"))
        $entries += @{ name = $name; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $manifest = @{ product = 'FlitzZip'; gpu_backend = @{ enabled = $true }; packaged_runtime_files = $entries }
    $manifestPath = Join-Path $fixture 'flitzzip-runtime-dependencies.json'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true
    $module = Join-Path $fixture 'bin/flitzzip_hip_kernels.dll'
    [IO.File]::AppendAllText($module, 'changed')
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    Remove-Item -LiteralPath $module
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    $manifest.packaged_runtime_files = @($entries[0], $entries[1], $entries[1])
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Assert-RuntimeRejected { Assert-FlitzZipPackagedRuntime -PackageRoot $fixture -HipEnabled $true }
    foreach ($state in @(
        @{ code = 0; ready = 'true'; runtime = 'true'; kernel = 'true' },
        @{ code = 11; ready = 'false'; runtime = 'false'; kernel = 'false' },
        @{ code = 12; ready = 'false'; runtime = 'true'; kernel = 'false' }
    )) {
        $output = @('hip_compiled=true', "available=$($state.ready)",
            "hip_runtime_loadable=$($state.runtime)", "hip_kernel_loadable=$($state.kernel)")
        Assert-FlitzZipHipDependencyState -Output $output -ExitCode $state.code
        Assert-FlitzZipHipDependencyState -Output ($output + '') -ExitCode $state.code
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output $output -ExitCode 13 }
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output ($output + 'available=false') -ExitCode $state.code }
        Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output $output[0..2] -ExitCode $state.code }
    }
    Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output @('hip_compiled=false') -ExitCode 10 }
    Assert-RuntimeRejected { Assert-FlitzZipHipDependencyState -Output @('hip_compiled=true', 'available=true') -ExitCode 11 }
    Test-WixVersionAdmission
    Test-WixEnvironmentOwnership
    Test-WixUiAdmission
    Test-PackageGenerationTransaction -Root $fixture
    Test-PortablePayloadContent -Root $fixture
    Test-PortablePayloadRejection -Root $fixture
    Test-PortablePublicationRecovery -Root $fixture
    Test-PortableManifestAdmission -Root $fixture
    Test-PayloadMemberNameIdentity -Root $fixture
    Test-InstalledPayloadParity -Root $fixture
    Test-PortableSmokeLifecycle -Root $fixture
    Write-Output 'Package integrity, truthful HIP capability and exact installed-payload ZIP contracts passed.'
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $fixture).Path)
        if ($resolved -cne [IO.Path]::GetFullPath($fixture) -or
            (Get-Item -LiteralPath $fixture -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing redirected runtime fixture cleanup.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
