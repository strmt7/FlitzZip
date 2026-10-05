# Purpose: Require immutable tool bytes before extraction or execution.
# Inputs: Path names an existing regular file; Expected is its pinned SHA-256.
# Outputs: Returns on an exact match; throws for missing, linked, or modified content.
function Assert-CMakeToolHash {
    param([string]$Path, [string]$Expected)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Pinned CMake tool file is missing or linked.'
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw 'Pinned CMake tool SHA-256 mismatch.'
    }
}

# Purpose: Load public tool provenance from a standard SHA-256 checksum manifest.
# Inputs: Path is the repository manifest or a private validation fixture.
# Outputs: Complete release, executable and ABI identities; rejects duplicate, missing or malformed records.
function Get-CMakeToolchainLock {
    param([string]$Path = (Join-Path $PSScriptRoot 'cmake-toolchain.sha256'))
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-Item -LiteralPath $Path).Length -gt 16384) {
        throw 'Pinned CMake provenance manifest is missing, linked or oversized.'
    }
    $records = @{}
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9_./-]+)$' -or $records.ContainsKey($Matches[2])) {
            throw 'Pinned CMake provenance record is malformed or duplicated.'
        }
        $records[$Matches[2]] = $Matches[1]
    }
    $archive = 'cmake-4.4.3-windows-x86_64.zip'
    $keys = @($archive, 'bin/cmake.exe', 'bin/cpack.exe', 'bin/ctest.exe',
        'Modules/CMakeCompilerABI.upstream.h', 'Modules/CMakeCompilerABI.guarded.h')
    if (@(Compare-Object $keys @($records.Keys)).Count -ne 0) {
        throw 'Pinned CMake provenance inventory is invalid.'
    }
    return [pscustomobject]@{
        version = '4.4.3'
        archive_sha256 = $records[$archive]
        executables_sha256 = [pscustomobject]@{
            'cmake.exe' = $records['bin/cmake.exe']
            'cpack.exe' = $records['bin/cpack.exe']
            'ctest.exe' = $records['bin/ctest.exe']
        }
        abi_header_original_sha256 = $records['Modules/CMakeCompilerABI.upstream.h']
        abi_header_patched_sha256 = $records['Modules/CMakeCompilerABI.guarded.h']
    }
}

# Purpose: Make the pinned ABI probe safe for repeated inclusion without altering its payload.
# Inputs: Install is an unlinked, verified CMake installation; admits only exact original or repaired bytes.
# Outputs: Atomically adds a complete header guard, or rejects drift, links and interrupted publication.
function Repair-CMakeCompilerAbiHeader {
    param([Parameter(Mandatory = $true)][string]$Install)
    $directory = $Install
    foreach ($part in @('share', 'cmake-4.4', 'Modules')) {
        $directory = Join-Path $directory $part
        if (-not (Test-Path -LiteralPath $directory -PathType Container) -or
            ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Pinned CMake ABI module directory is missing or linked.'
        }
    }
    $path = Join-Path $directory 'CMakeCompilerABI.h'
    $lock = Get-CMakeToolchainLock
    $original = $lock.abi_header_original_sha256
    $repaired = $lock.abi_header_patched_sha256
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Pinned CMake ABI header is missing or linked.'
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -eq $repaired) { return }
    Assert-CMakeToolHash -Path $path -Expected $original
    $body = [IO.File]::ReadAllText($path)
    $content = "#ifndef SUPERZIP_CMAKE_COMPILER_ABI_H`n#define SUPERZIP_CMAKE_COMPILER_ABI_H`n" +
        $body + "`n#endif /* SUPERZIP_CMAKE_COMPILER_ABI_H */`n"
    $temporary = "$path.superzip-abi"
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($content)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    try {
        Assert-CMakeToolHash -Path $temporary -Expected $repaired
        Assert-CMakeToolHash -Path $path -Expected $original
        [IO.File]::Replace($temporary, $path, [NullString]::Value)
        Assert-CMakeToolHash -Path $path -Expected $repaired
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

# Purpose: Provision the same stable Windows x64 CMake toolchain for local and hosted builds.
# Inputs: RepoRoot is the checkout root; uses the pinned release and reviewed ABI header repair.
# Outputs: Returns verified cmake.exe and guarded module bytes; never changes host installations or PATH.
function Find-CMake {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $lock = Get-CMakeToolchainLock
    $version = $lock.version
    $root = [IO.Path]::GetFullPath($RepoRoot)
    $out = Join-Path $root 'out'
    $tools = Join-Path $out 'tools'
    $cache = Join-Path $tools "cmake-$version"
    foreach ($directory in @($out, $tools, $cache)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        if ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing a linked CMake tool cache.'
        }
    }
    $package = "cmake-$version-windows-x86_64"
    $install = Join-Path $cache $package
    if (-not (Test-Path -LiteralPath $install)) {
        $archive = Join-Path $cache "$package.zip"
        $expected = $lock.archive_sha256
        if (-not (Test-Path -LiteralPath $archive)) {
            $partial = "$archive.partial"
            if (Test-Path -LiteralPath $partial) { throw 'An incomplete CMake download already exists.' }
            try {
                Invoke-WebRequest -Uri "https://github.com/Kitware/CMake/releases/download/v$version/$package.zip" `
                    -OutFile $partial -TimeoutSec 120
                Assert-CMakeToolHash -Path $partial -Expected $expected
                Move-Item -LiteralPath $partial -Destination $archive
            } finally {
                if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
            }
        }
        Assert-CMakeToolHash -Path $archive -Expected $expected
        $stage = Join-Path $cache ([guid]::NewGuid().ToString('N'))
        Expand-Archive -LiteralPath $archive -DestinationPath $stage
        Move-Item -LiteralPath (Join-Path $stage $package) -Destination $install
        Remove-Item -LiteralPath $stage
    }
    if ((Get-Item -LiteralPath $install).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing a linked CMake installation.'
    }
    $bin = Join-Path $install 'bin'
    if ((Get-Item -LiteralPath $bin).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing a linked CMake binary directory.'
    }
    foreach ($tool in @('cmake.exe', 'cpack.exe', 'ctest.exe')) {
        Assert-CMakeToolHash -Path (Join-Path $bin $tool) -Expected $lock.executables_sha256.$tool
    }
    Repair-CMakeCompilerAbiHeader -Install $install
    return Join-Path $bin 'cmake.exe'
}

# Purpose: Choose a supported generator without switching an existing CMake build tree.
# Inputs: Requested and Cached are generator names or empty; InstalledMajors lists discovered VS versions.
# Outputs: Returns VS 2022/2026's generator; rejects mismatched caches or unavailable installations.
function Select-CMakeGenerator {
    param([string]$Requested, [string]$Cached, [int[]]$InstalledMajors)
    $supported = @{ 'Visual Studio 17 2022' = 17; 'Visual Studio 18 2026' = 18 }
    if ($Requested -and $Cached -and $Requested -ne $Cached) {
        throw 'CMake generator differs from the existing build tree; preserve it and configure a fresh tree.'
    }
    $selected = if ($Requested) { $Requested } else { $Cached }
    if (-not $selected) {
        if ($InstalledMajors -contains 18) { $selected = 'Visual Studio 18 2026' }
        elseif ($InstalledMajors -contains 17) { $selected = 'Visual Studio 17 2022' }
    }
    if (-not $selected -or -not $supported.ContainsKey($selected) -or
        $InstalledMajors -notcontains $supported[$selected]) {
        throw 'The selected CMake generator requires an installed VS 2022 or VS 2026 C++ toolchain.'
    }
    return $selected
}

# Purpose: Discover Windows C++ installations and preserve the checkout's configured generator.
# Inputs: Requested is an explicit generator or empty; BuildRoot is the existing or proposed build directory.
# Outputs: Returns a supported installed generator; propagates discovery and cache mismatch failures.
function Find-CMakeGenerator {
    param([string]$Requested, [string]$BuildRoot)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio discovery tool is missing.' }
    $versions = & $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio installation discovery failed.' }
    $majors = @($versions | ForEach-Object { ([version]$_).Major })
    $cached = ''
    $cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $generator = @(Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$')
        if ($generator.Count -ne 1) { throw 'CMake build cache has no unique configured generator.' }
        $cached = $generator[0].Matches[0].Groups[1].Value
    }
    return Select-CMakeGenerator -Requested $Requested -Cached $cached -InstalledMajors $majors
}
