# 构建 BlockStorage（CMake + MSVC，便携工具链）
# 用法：  powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 [-Clean] [-Config Release]
param(
    [switch]$Clean,
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')

$cmake = Join-Path $RepoRoot 'tools\cmake\bin\cmake.exe'
$build = Join-Path $RepoRoot 'build'

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

# 便携工具链缺 oldnames.lib：用 lib.exe 造一个空壳（仅满足默认库引用）
$ucrtLib = Get-ChildItem (Join-Path $RepoRoot 'tools\sdk\Lib') -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1 |
    ForEach-Object { Join-Path $_.FullName 'ucrt\x64' }
if ($ucrtLib -and (Test-Path $ucrtLib) -and -not (Test-Path (Join-Path $ucrtLib 'oldnames.lib'))) {
    $def = Join-Path $env:TEMP 'oldnames.def'
    $outLib = Join-Path $ucrtLib 'oldnames.lib'
    Set-Content -Path $def -Value "LIBRARY oldnames`nEXPORTS" -Encoding ascii
    & lib.exe /nologo /def:$def /out:$outLib /machine:x64 | Out-Null
    if (Test-Path $outLib) {
        Write-Host "generated $outLib" -ForegroundColor DarkGray
    } else {
        Write-Warning 'failed to generate oldnames.lib'
    }
}

# 生成器：优先 Ninja，否则 NMake
$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
$nmake = Get-Command nmake.exe -ErrorAction SilentlyContinue
if ($ninja) {
    $generator = 'Ninja'
} elseif ($nmake) {
    $generator = 'NMake Makefiles'
} else {
    throw 'Ninja 与 nmake 均不可用，请检查 MSVC 工具包'
}

if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

# 静态 CRT（/MT）：便携工具链没有 msvcprt.lib / vcruntime.lib 导入库
$argsList = @(
    '-S', $RepoRoot,
    '-B', $build,
    '-G', $generator,
    "-DCMAKE_BUILD_TYPE=$Config",
    '-DCMAKE_C_COMPILER=cl',
    '-DCMAKE_CXX_COMPILER=cl',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_MSVC_RUNTIME_LIBRARY'
)
Write-Host "== configure ($generator) ==" -ForegroundColor Cyan
& $cmake @argsList
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host "== build ==" -ForegroundColor Cyan
& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$exe = Get-ChildItem $build -Recurse -Filter 'BlockStorage.exe' | Select-Object -First 1
if ($exe) {
    Write-Host "== built: $($exe.FullName)" -ForegroundColor Green
    return $exe.FullName
}
throw 'BlockStorage.exe not found after build'
