param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build/vs2022-win32'
& cmake -S $Root -B $Build -G 'Visual Studio 17 2022' -A Win32 -DOGG_INPUT_DEPENDENCIES_ONLY=OFF "-DCMAKE_INSTALL_PREFIX=$Root/dist"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $Build --config $Configuration --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    & ctest --test-dir $Build -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
if ($Package) {
    if ($Configuration -ne 'Release') { throw 'Only Release builds are packaged.' }
    & cmake --install $Build --config Release --component Runtime
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
}
