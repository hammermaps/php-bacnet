param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

$stackRoot = Join-Path $SourceRoot 'deps\bacnet-stack'
$buildRoot = Join-Path $stackRoot 'build-windows'

if (-not (Test-Path (Join-Path $stackRoot 'CMakeLists.txt'))) {
    throw 'bacnet-stack source missing; checkout submodules recursively.'
}

cmake -S $stackRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64 `
    -DBUILD_SHARED_LIBS=OFF `
    -DBACNET_STACK_BUILD_APPS=OFF `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildRoot --config Release --parallel
exit $LASTEXITCODE
