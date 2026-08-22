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
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual C++ build tools missing.' }
$vcvars = Join-Path $vsPath 'VC\\Auxiliary\\Build\\vcvars64.bat'
$mdb = Join-Path $lmdbRoot 'mdb.c'
$midl = Join-Path $lmdbRoot 'midl.c'
$mdbObj = Join-Path $lmdbBuildRoot 'mdb.obj'
$midlObj = Join-Path $lmdbBuildRoot 'midl.obj'
$lmdbLib = Join-Path $lmdbBuildRoot 'lmdb.lib'
$command = "`"$vcvars`" && cl.exe /nologo /O2 /MD /c `"$mdb`" /Fo`"$mdbObj`" && cl.exe /nologo /O2 /MD /c `"$midl`" /Fo`"$midlObj`" && lib.exe /nologo /OUT:`"$lmdbLib`" `"$mdbObj`" `"$midlObj`""
cmd.exe /d /s /c $command
exit $LASTEXITCODE
