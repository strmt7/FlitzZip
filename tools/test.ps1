$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
$cmakeCTest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if (Test-Path $cmakeCTest) {
    $ctestExe = $cmakeCTest
} elseif ($ctest) {
    $ctestExe = $ctest.Source
} else {
    throw "ctest was not found."
}
& $ctestExe --test-dir (Join-Path $repo "build") -C Release --output-on-failure
