# 便携工具链环境（无需管理员，全部位于 <repo>\tools）
# 用法：  . .\scripts\env.ps1
$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:Tools    = Join-Path $script:RepoRoot 'tools'

function Find-Dir {
    param([string]$Path, [string]$Filter = '*')
    if (-not (Test-Path $Path)) { return $null }
    Get-ChildItem $Path -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like $Filter } |
        Sort-Object Name -Descending |
        Select-Object -First 1
}

# ---------- MSVC ----------
$msvcBase = Join-Path $script:Tools 'msvc\VC\Tools\MSVC'
$msvc = $null
if (Test-Path $msvcBase) {
    $msvc = Get-ChildItem $msvcBase -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\HostX64\x64\cl.exe') } |
        Sort-Object Name -Descending |
        Select-Object -First 1
}

# ---------- Windows SDK (NuGet 版) ----------
$sdkIncRoot = Join-Path $script:Tools 'sdk\Include'
$sdkLibRoot = Join-Path $script:Tools 'sdk\Lib'
$sdkVer = Find-Dir $sdkIncRoot
$sdkLibVer = Find-Dir $sdkLibRoot

# ---------- CMake ----------
$cmakeBin = Join-Path $script:Tools 'cmake\bin'

if (-not $msvc) { Write-Warning 'MSVC 未找到：先运行 python scripts\fetch_msvc.py msvc' }
if (-not $sdkVer) { Write-Warning 'Windows SDK 未找到：先运行 python scripts\fetch_msvc.py sdk' }

$env:VCToolsInstallDir = if ($msvc) { "$($msvc.FullName)\" } else { '' }

$incParts = @()
$libParts = @()
if ($msvc) { $incParts += (Join-Path $msvc.FullName 'include'); $libParts += (Join-Path $msvc.FullName 'lib\x64') }
if ($sdkVer) {
    foreach ($d in 'ucrt', 'um', 'shared', 'winrt', 'cppwinrt') {
        $p = Join-Path $sdkVer.FullName $d
        if (Test-Path $p) { $incParts += $p }
    }
}
if ($sdkLibVer) {
    foreach ($d in 'ucrt\x64', 'um\x64') {
        $p = Join-Path $sdkLibVer.FullName $d
        if (Test-Path $p) { $libParts += $p }
    }
}
$env:INCLUDE = ($incParts -join ';')
$env:LIB     = ($libParts -join ';')

$pathParts = @()
if ($msvc) { $pathParts += (Join-Path $msvc.FullName 'bin\HostX64\x64') }
$pathParts += $cmakeBin
$pathParts += (Join-Path $script:Tools 'sdk\bin')
$pathParts += (Join-Path $script:Tools 'ninja')
$env:PATH = (($pathParts + $env:PATH) -join ';')

$env:CC  = 'cl'
$env:CXX = 'cl'

# BlockStorage 默认导出根目录（程序未设置时的默认值是 C:\BlockStorage\extract）
# 这里指向仓库内的 out\extract，便于开发/测试时不污染系统盘
if (-not $env:BLOCKSTORAGE_OUT) {
    $script:OutRoot = Join-Path $script:RepoRoot 'out\extract'
    $env:BLOCKSTORAGE_OUT = $script:OutRoot
}

Write-Host "MSVC    : $(if ($msvc) { $msvc.FullName } else { '<missing>' })" -ForegroundColor DarkGray
Write-Host "SDK     : $(if ($sdkVer) { $sdkVer.FullName } else { '<missing>' })" -ForegroundColor DarkGray
Write-Host "CMake   : $(Join-Path $cmakeBin 'cmake.exe')" -ForegroundColor DarkGray
Write-Host "BS out  : $env:BLOCKSTORAGE_OUT" -ForegroundColor DarkGray
