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

# Purpose: Provision the same stable Windows x64 CMake toolchain for local and hosted builds.
# Inputs: RepoRoot is the checkout root; uses Kitware's pinned 4.4.3 release and ignored out cache.
# Outputs: Returns verified cmake.exe; downloads only on a cache miss and never changes host installations or PATH.
function Find-CMake {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $version = '4.4.3'
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
        $expected = '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab'
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
    foreach ($tool in @(
        @('cmake.exe', 'AB8247CA4554871E5D75C0EE118EE8663727BD6FDF37AD76B17E9CC8D5F286A8'),
        @('cpack.exe', '5B096261D79B67945B0070030E0E4AC7F276E0641F3A52CF72ACE038D77D4371'),
        @('ctest.exe', '083F482C8656C86932C28906445CB3C10C0C5EE649A90FF3E0080EE4447D3DFC')
    )) {
        Assert-CMakeToolHash -Path (Join-Path $bin $tool[0]) -Expected $tool[1]
    }
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
