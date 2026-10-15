# Configure + build Living World. CMake is taken from PATH or from Visual Studio.
#
#   scripts\build.ps1                 build RelWithDebInfo
#   scripts\build.ps1 -Run            build and start the app
#   scripts\build.ps1 -Test           build and run tests
#   scripts\build.ps1 -Config Debug   another configuration
#   scripts\build.ps1 -Clean          delete build/ first
#
# (ASCII only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.)
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Run,
    [switch]$Test,
    [switch]$Clean
)

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'

function Find-CMakeBin {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return Split-Path -Parent $onPath.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath) {
            $bin = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
            if (Test-Path (Join-Path $bin 'cmake.exe')) { return $bin }
        }
    }
    return $null
}

$cmakeBin = Find-CMakeBin
if (-not $cmakeBin) {
    Write-Host 'cmake not found: install CMake or the "C++ CMake tools" component of Visual Studio.'
    exit 1
}
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Push-Location $root
    & $cmake --preset vs
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { exit $code }
}

& $cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Run) {
    & (Join-Path $buildDir "bin\$Config\living_world.exe")
}
