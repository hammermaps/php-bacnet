param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

$stackRoot = Join-Path $SourceRoot 'deps\bacnet-stack'
$buildRoot = Join-Path $stackRoot 'build-windows'
$lmdbRoot = Join-Path $SourceRoot 'deps\lmdb\libraries\liblmdb'
$lmdbBuildRoot = Join-Path $SourceRoot 'deps\lmdb\build-windows'

if (-not (Test-Path (Join-Path $stackRoot 'CMakeLists.txt'))) {
    throw 'bacnet-stack source missing; checkout submodules recursively.'
}
if (-not (Test-Path (Join-Path $lmdbRoot 'mdb.c'))) {
    throw 'LMDB source missing; checkout submodules recursively.'
}

cmake -S $stackRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64 `
    -DBUILD_SHARED_LIBS=OFF `
    -DBACNET_STACK_BUILD_APPS=OFF `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildRoot --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force -Path $lmdbBuildRoot | Out-Null
& cl.exe /nologo /O2 /MD /c (Join-Path $lmdbRoot 'mdb.c') /Fo(Join-Path $lmdbBuildRoot 'mdb.obj')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cl.exe /nologo /O2 /MD /c (Join-Path $lmdbRoot 'midl.c') /Fo(Join-Path $lmdbBuildRoot 'midl.obj')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& lib.exe /nologo /OUT:(Join-Path $lmdbBuildRoot 'lmdb.lib') (Join-Path $lmdbBuildRoot 'mdb.obj') (Join-Path $lmdbBuildRoot 'midl.obj')
exit $LASTEXITCODE
