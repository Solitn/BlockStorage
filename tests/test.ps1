# BlockStorage 端到端测试
# 用法： pwsh -ExecutionPolicy Bypass -File .\tests\test.ps1 [-Exe <path>] [-Keep]
param(
    [string]$Exe = '',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
# 管道写入子进程的编码：Windows PowerShell 5.1 默认 ASCII，会让中文变成 '?'
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $Exe = Get-ChildItem (Join-Path $RepoRoot 'build') -Recurse -Filter 'BlockStorage.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Exe -or -not (Test-Path $Exe)) { throw 'BlockStorage.exe not found; build first (scripts\build.ps1)' }

$Work = Join-Path $PSScriptRoot '_run'
$pass = 0
$fail = 0
$Pwd0 = 'S3cret!'

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        $script:pass++
        Write-Host ("  PASS  " + $name) -ForegroundColor Green
    } else {
        $script:fail++
        Write-Host ("  FAIL  " + $name + "  " + $detail) -ForegroundColor Red
    }
}

# 每次启动都是新进程：默认先 load 解锁
function Run-Repo {
    param(
        [string[]]$Lines,
        [string]$Dir = $Work,
        [switch]$NoLoad
    )
    $all = @()
    if (-not $NoLoad) { $all += "load -p$Pwd0" }
    $all += $Lines

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $Dir
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    foreach ($l in $all) { $p.StandardInput.WriteLine($l) }
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    return $out
}

function New-Sample([string]$path, [int]$size) {
    $bytes = New-Object byte[] $size
    for ($i = 0; $i -lt $size; $i++) { $bytes[$i] = [byte](($i * 31 + 7) % 251) }
    [IO.File]::WriteAllBytes($path, $bytes)
}

# ---------- 准备 ----------
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Path $Work, (Join-Path $Work 'src'), (Join-Path $Work 'out') -Force | Out-Null

# 默认导出根目录走环境变量（避免写到 C:\），同时测试 env 优先级
$env:BLOCKSTORAGE_OUT = Join-Path $Work 'dropped'

$sizes = [ordered]@{ 'empty.bin' = 0; 'one.bin' = 1; 's511.bin' = 511; 's512.bin' = 512;
                     's513.bin' = 513; 's4096.bin' = 4096; 's4097.bin' = 4097; 'big.bin' = 1048576 }
foreach ($k in $sizes.Keys) { New-Sample (Join-Path $Work "src\$k") $sizes[$k] }

Write-Host "== init ==" -ForegroundColor Cyan
$out = Run-Repo -NoLoad @('init -pS3cret!', 'statu', 'exit')
Check 'init reports success' ($out -match 'Repository initialized')
Check 'repository created' (Test-Path (Join-Path $Work '.block\data.blk'))
Check 'statu shows haskey' ($out -match 'haskey\s+:\s+true')
Check 'statu shows hasload' ($out -match 'hasload\s+:\s+true')

Write-Host "== add ==" -ForegroundColor Cyan
$lines = @()
foreach ($k in $sizes.Keys) { $lines += "add src\$k" }
$lines += 'ls'
$out = Run-Repo ($lines + 'exit')
foreach ($k in $sizes.Keys) { Check "add $k" ($out -match [regex]::Escape("Added /$k")) }
$lsRows = ($out -split "`n") | Where-Object { $_ -match '^file\s+\d+\s+/' }
Check 'ls lists 8 files' ($lsRows.Count -eq 8) "got $($lsRows.Count)"

Write-Host "== get (round-trip) ==" -ForegroundColor Cyan
$lines = @()
foreach ($k in $sizes.Keys) { $lines += "get $k out\$k" }
$out = Run-Repo ($lines + 'exit')
foreach ($k in $sizes.Keys) {
    $src = Join-Path $Work "src\$k"
    $dst = Join-Path $Work "out\$k"
    $same = (Test-Path $dst) -and ((Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash)
    Check "round-trip $k ($($sizes[$k]) B)" $same
}

Write-Host "== duplicate / missing ==" -ForegroundColor Cyan
$out = Run-Repo @('add src\one.bin', 'get nope.bin x.bin', 'rm nope.bin', 'exit')
Check 'duplicate rejected' ($out -match 'already exists')
Check 'missing get rejected' ($out -match 'No such file')
Check 'missing rm rejected' ($out -match 'No such file')

Write-Host "== delete / remove ==" -ForegroundColor Cyan
$out = Run-Repo @('rm big.bin', 'ls', 'exit')
Check 'rm works' ($out -match 'Removed /big.bin')
$lsRows = ($out -split "`n") | Where-Object { $_ -match '^file\s+\d+\s+/' }
Check 'ls no longer lists big.bin' (-not ($lsRows -match '/big\.bin'))

Write-Host "== lock / unlock ==" -ForegroundColor Cyan
$out = Run-Repo -NoLoad @('ls', 'exit')
Check 'locked repo rejects ls' ($out -match 'locked')
$out = Run-Repo -NoLoad @('load -pWrongPass', 'exit')
Check 'wrong password rejected' ($out -match 'Incorrect password')
$out = Run-Repo @('ls', 'exit')
Check 'correct password unlocks' ($out -match 'Loaded repository')
Check 'ls works after unlock' ($out -match 'one\.bin')

Write-Host "== tamper detection ==" -ForegroundColor Cyan
$data = Join-Path $Work '.block\data.blk'
$bytes = [IO.File]::ReadAllBytes($data)
Check 'data.blk has content' ($bytes.Length -gt 4200)
if ($bytes.Length -gt 4200) {
    # 块 1（偏移 4124 起）属于 s4096.bin
    $off = 4124 + 100
    $bytes[$off] = $bytes[$off] -bxor 0xFF
    [IO.File]::WriteAllBytes($data, $bytes)
    $out = Run-Repo @('get s4096.bin out\tampered.bin', 'exit')
    Check 'tampered block detected' ($out -match 'authentication failed|Checksum mismatch')
    $bytes[$off] = $bytes[$off] -bxor 0xFF
    [IO.File]::WriteAllBytes($data, $bytes)
    $out = Run-Repo @('get one.bin out\one2.bin', 'exit')
    Check 'restored data readable' ((Test-Path (Join-Path $Work 'out\one2.bin')))
}

Write-Host "== directory tree (内部路径) ==" -ForegroundColor Cyan
$tree = Join-Path $Work 'tree'
New-Item -ItemType Directory -Path (Join-Path $tree 'sub\deep'), (Join-Path $tree 'empty') -Force | Out-Null
New-Sample (Join-Path $tree 'a.txt') 10
New-Sample (Join-Path $tree 'sub\b.bin') 6000
New-Sample (Join-Path $tree 'sub\deep\c.txt') 3

# 1) 整个文件夹 -> 内部路径 /archive/tree
$out = Run-Repo @("add $tree /archive/tree", 'ls /archive -r', 'exit')
Check 'import folder keeps tree' ($out -match 'Imported 3 file\(s\), \d+ dir\(s\) into /archive/tree')
Check 'ls -r shows nested paths' (($out -match '/archive/tree/sub/deep/c\.txt') -and ($out -match '/archive/tree/sub/b\.bin'))

# 2) 文件 -> 指定内部文件名
$out = Run-Repo @("add $tree\a.txt /notes/hello.txt", 'ls /notes', 'exit')
Check 'add file to explicit path' ($out -match 'Added /notes/hello\.txt')

# 3) 文件 -> 放入某目录（尾部斜杠）
$out = Run-Repo @("add $tree\a.txt /notes/", 'ls /notes -r', 'exit')
Check 'add file into folder' ($out -match 'Added /notes/a\.txt')

# 4) 反向：整棵目录导出到指定目录
$restoreA = Join-Path $Work 'restoreA'
$out = Run-Repo @("get /archive $restoreA", 'exit')
Check 'export folder reports count' ($out -match 'Extracted 3 file\(s\), \d+ dir\(s\) from /archive')
Check 'exported nested file exists' (Test-Path (Join-Path $restoreA 'tree\sub\deep\c.txt'))
Check 'exported content matches' ((Get-FileHash (Join-Path $tree 'sub\b.bin')).Hash -eq
                                  (Get-FileHash (Join-Path $restoreA 'tree\sub\b.bin')).Hash)

# 5) 反向：不带目标 -> 使用默认导出根（环境变量 BLOCKSTORAGE_OUT）
$out = Run-Repo @('get /archive/tree/sub/deep/c.txt', 'exit')
$defaultFile = Join-Path $Work 'dropped\archive\tree\sub\deep\c.txt'
Check 'default out root used' (Test-Path $defaultFile)

# 6) out 命令：显示 / 设置
$out = Run-Repo @('out', 'exit')
Check 'out shows env source' ($out -match 'BLOCKSTORAGE_OUT')
$cfgOut = Join-Path $Work 'cfgout'
$saved = $env:BLOCKSTORAGE_OUT
Remove-Item Env:\BLOCKSTORAGE_OUT -ErrorAction SilentlyContinue
$out = Run-Repo -NoLoad @("load -p$Pwd0", "out $cfgOut", 'out', 'exit')
Check 'out sets config' ($out -match 'Default extract root set')
Check 'config file written' (Test-Path (Join-Path $Work '.block\config.json'))
Check 'config source reported' ($out -match '\.block/config\.json')
$env:BLOCKSTORAGE_OUT = $saved

# 7) rm 目录：不带 -r 拒绝，带 -r 删除
$out = Run-Repo @('rm /archive', 'rm /archive -r', 'ls /archive -r', 'exit')
Check 'rm dir needs -r' ($out -match 'is a directory.*use rm -r')
Check 'rm -r removes subtree' ($out -match 'Removed \d+ entries under /archive')

# 8) 非法内部路径
$out = Run-Repo @("add $tree\a.txt /a/../b", 'exit')
Check 'path traversal rejected' ($out -match 'Invalid internal path')

Write-Host "== import（文件 + 目录，含空目录）==" -ForegroundColor Cyan
$mirror = Join-Path $Work 'mirror'
New-Item -ItemType Directory -Path (Join-Path $mirror 'src\empty'), (Join-Path $mirror 'emptydir') -Force | Out-Null
New-Sample (Join-Path $mirror 'src\a.cpp') 12
New-Sample (Join-Path $mirror 'root.txt') 5

$out = Run-Repo @("import $mirror /m", 'ls /m -r', 'exit')
Check 'import reports files and dirs' ($out -match 'Imported 2 file\(s\), 4 dir\(s\)')
Check 'import creates dir entries' ($out -match 'dir\s+-\s+/m/emptydir/')
Check 'import keeps nested empty dir' ($out -match '/m/src/empty/')

$mirrorOut = Join-Path $Work 'mirror-restore'
$out = Run-Repo @("get /m $mirrorOut", 'exit')
Check 'export counts files and dirs' ($out -match 'Extracted 2 file\(s\), 3 dir\(s\)')
Check 'empty dir restored' (Test-Path (Join-Path $mirrorOut 'emptydir'))
Check 'nested empty dir restored' (Test-Path (Join-Path $mirrorOut 'src\empty'))
Check 'file restored' ((Get-FileHash (Join-Path $mirror 'root.txt')).Hash -eq
                       (Get-FileHash (Join-Path $mirrorOut 'root.txt')).Hash)

# 目录单独导出：精确命中目录条目时也应导出整棵树
$onlyEmpty = Join-Path $Work 'only-empty'
$out = Run-Repo @("get /m/emptydir $onlyEmpty", 'exit')
Check 'exact dir export works' ($out -match 'Created directory|Extracted 0 file\(s\)')

$out = Run-Repo @('rm /m', 'rm /m -r', 'exit')
Check 'rm dir needs -r (entries)' ($out -match 'is a directory .*entries.*use rm -r')
Check 'rm -r removes tree' ($out -match 'Removed 6 entries under /m')

Write-Host "== 中文路径 ==" -ForegroundColor Cyan
$cn = Join-Path $Work '中文目录'
New-Item -ItemType Directory -Path (Join-Path $cn '子目录') -Force | Out-Null
Set-Content -Path (Join-Path $cn '说明.txt') -Value '中文内容' -Encoding UTF8
Set-Content -Path (Join-Path $cn '子目录\代码.cpp') -Value 'int main(){}' -Encoding UTF8

$out = Run-Repo @("import $cn /备份/中文目录", 'ls /备份 -r', 'exit')
Check 'import chinese dir' ($out -match 'Imported 2 file\(s\)')
Check 'ls shows chinese paths' ($out -match '/备份/中文目录/子目录/代码\.cpp')

$cnOut = Join-Path $Work '中文还原'
$out = Run-Repo @("get /备份 $cnOut", 'exit')
$cnFile = Join-Path $cnOut '中文目录\子目录\代码.cpp'
Check 'chinese round-trip' ((Test-Path $cnFile) -and
                            ((Get-Content $cnFile -Raw) -eq (Get-Content (Join-Path $cn '子目录\代码.cpp') -Raw)))

Write-Host "== 版本号 ==" -ForegroundColor Cyan
$out = Run-Repo -NoLoad @('exit')
Check 'banner shows v1.0.0' ($out -match 'BlockStorage v1\.0\.0')

Write-Host "== no-password repository ==" -ForegroundColor Cyan
$plain = Join-Path $Work 'plain'
New-Item -ItemType Directory -Path $plain -Force | Out-Null
New-Sample (Join-Path $plain 'a.bin') 100
$out = Run-Repo -NoLoad -Dir $plain @('init', 'add a.bin', 'get a.bin a.out', 'exit')
Check 'keyless init ok' ($out -match 'no password')
Check 'keyless add ok' ($out -match 'Added /a\.bin')
Check 'keyless round-trip' ((Get-FileHash (Join-Path $plain 'a.bin')).Hash -eq (Get-FileHash (Join-Path $plain 'a.out')).Hash)

Write-Host "== delete repository ==" -ForegroundColor Cyan
$out = Run-Repo @('delete', 'statu', 'exit')
Check 'delete works' ($out -match 'Repository deleted')
Check '.block removed' (-not (Test-Path (Join-Path $Work '.block')))

Write-Host ""
Write-Host ("PASS=$pass FAIL=$fail") -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if (-not $Keep -and (Test-Path $Work)) { Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue }
if ($fail -gt 0) { exit 1 }
