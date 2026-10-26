$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
. (Join-Path $PSScriptRoot 'cmake_toolchain.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$cmake = Find-CMake -RepoRoot $repo
$policy = Join-Path $repo 'cmake/FlitzZipMsiVersion.cmake'
foreach ($value in @('0.0.0', '0.8.0', '1.0.0', '31.4.128', '255.255.65535')) {
    if ((Assert-FlitzZipMsiPackageVersion -PackageVersion $value) -cne $value) { throw 'MSI version changed.' }
    & $cmake '-DCPACK_GENERATOR=WIX' "-DCPACK_FLITZZIP_PACKAGE_VERSION=$value" -P $policy
    if ($LASTEXITCODE -ne 0) { throw "CPack rejected a valid MSI version: $value" }
}
foreach ($value in @('256.0.0', '1.256.0', '1.0.65536', '99999999999999999999.0.0',
        '1.0.0-beta.1', '1.0.0-rc.1', '1.0.0.1', '01.0.0', 'v1.0.0', 'latest')) {
    $rejected = $false
    try { $null = Assert-FlitzZipMsiPackageVersion -PackageVersion $value } catch { $rejected = $true }
    if (-not $rejected) { throw "Unsafe installer version accepted: $value" }
    # Windows PowerShell 5 treats redirected native stderr as error records.
    # Capture this deliberate rejection, then require both its exit status and policy diagnostic.
    $savedErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & $cmake '-DCPACK_GENERATOR=WIX' "-DCPACK_FLITZZIP_PACKAGE_VERSION=$value" -P $policy 2>&1
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedErrorAction }
    if ($nativeExit -eq 0) { throw "Native CPack policy accepted an unsafe version: $value" }
    if (($output -join "`n") -notmatch 'MSI (requires|version)') { throw 'Version rejection did not come from MSI policy.' }
}
& $cmake '-DCPACK_GENERATOR=ZIP' '-DCPACK_FLITZZIP_PACKAGE_VERSION=1.0.0-beta.1' -P $policy
if ($LASTEXITCODE -ne 0) { throw 'Portable developer builds lost SemVer support.' }
$global:LASTEXITCODE = 0
Write-Output 'MSI numeric ordering and bounds contracts passed.'
