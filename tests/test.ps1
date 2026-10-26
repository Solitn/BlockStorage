# BlockStorage 端到端测试（索引 v5：目录树 + bit 链）
# 用法： powershell -ExecutionPolicy Bypass -File .\tests\test.ps1 [-Exe <path>] [-Keep]
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
    # ((i*31+7) % 251) 以 251 为周期；用整周期块拼接，避免大文件逐字节循环
    $period = 251
    $chunk = New-Object byte[] ($period * 4096)
    for ($i = 0; $i -lt $chunk.Length; $i++) { $chunk[$i] = [byte](($i * 31 + 7) % 251) }
    $bytes = New-Object byte[] $size
    $off = 0
    while ($off -lt $size) {
        $n = [Math]::Min($chunk.Length, $size - $off)
        [Array]::Copy($chunk, 0, $bytes, $off, $n)
        $off += $n
    }
    [IO.File]::WriteAllBytes($path, $bytes)
}

# ---------- 准备 ----------
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Path $Work, (Join-Path $Work 'src'), (Join-Path $Work 'out') -Force | Out-Null

$env:BLOCKSTORAGE_OUT = Join-Path $Work 'dropped'

$sizes = [ordered]@{ 'empty.bin' = 0; 'one.bin' = 1; 's511.bin' = 511; 's512.bin' = 512;
                     's513.bin' = 513; 's4096.bin' = 4096; 's4097.bin' = 4097; 'big.bin' = 1048576 }
foreach ($k in $sizes.Keys) { New-Sample (Join-Path $Work "src\$k") $sizes[$k] }

Write-Host "== init ==" -ForegroundColor Cyan
$out = Run-Repo -NoLoad @('init -pS3cret!', 'statu', 'exit')
Check 'init reports success' ($out -match 'Repository initialized')
Check 'repository has 5 files' ((Get-ChildItem (Join-Path $Work '.block') -File).Count -eq 5)
foreach ($f in 'idx', 'key', 'data.blk', 'data.small', 'bit') {
    Check "file $f exists" (Test-Path (Join-Path $Work ".block\$f"))
}
Check 'no config.json' (-not (Test-Path (Join-Path $Work '.block\config.json')))
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
Check 'index is encrypted (no plain names)' (-not ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $Work '.block\idx'))) -match 'big\.bin'))
# bit：96 字节头 + 每块 4 字节；此时小块 3 块（s4096、s4097x2）+ 大块 4 块
$bitLen = (Get-Item (Join-Path $Work '.block\bit')).Length
Check 'bit node is 4 bytes' ($bitLen -eq (96 + 4 * 7)) "got $bitLen"

Write-Host "== get (round-trip, structure kept) ==" -ForegroundColor Cyan
$lines = @()
foreach ($k in $sizes.Keys) { $lines += "get $k out\$k" }
$out = Run-Repo ($lines + 'exit')
foreach ($k in $sizes.Keys) {
    $src = Join-Path $Work "src\$k"
    $dst = Join-Path $Work "out\$k"
    $same = (Test-Path $dst) -and ((Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash)
    Check "round-trip $k ($($sizes[$k]) B)" $same
}
# 目标是已存在的目录：保留仓库内完整路径
$dirTarget = Join-Path $Work 'outdir'
New-Item -ItemType Directory -Path $dirTarget -Force | Out-Null
$out = Run-Repo @('get /one.bin outdir', 'exit')
Check 'dir target keeps full path' (Test-Path (Join-Path $dirTarget 'one.bin'))

Write-Host "== duplicate / missing ==" -ForegroundColor Cyan
$out = Run-Repo @('add src\one.bin', 'get nope.bin x.bin', 'rm nope.bin', 'exit')
Check 'duplicate rejected' ($out -match 'Duplicate path')
Check 'missing get rejected' ($out -match 'No such file')
Check 'missing rm rejected' ($out -match 'No such file')

Write-Host "== delete / remove + block reuse ==" -ForegroundColor Cyan
$out = Run-Repo @('rm big.bin', 'ls', 'statu', 'exit')
Check 'rm works' ($out -match 'Removed /big.bin')
$lsRows = ($out -split "`n") | Where-Object { $_ -match '^file\s+\d+\s+/' }
Check 'ls no longer lists big.bin' (-not ($lsRows -match '/big\.bin'))
Check 'blocks are freed' ($out -match 'big blocks : 0 / 4 used')

$out = Run-Repo @('add src\big.bin /big.bin', 'statu', 'exit')
Check 'freed middle blocks reused' ($out -match 'big blocks : 4 / 4 used')

Write-Host "== lock / unlock ==" -ForegroundColor Cyan
$out = Run-Repo -NoLoad @('ls', 'exit')
Check 'locked repo rejects ls' ($out -match 'locked')
$out = Run-Repo -NoLoad @('load -pWrongPass', 'exit')
Check 'wrong password rejected' ($out -match 'Incorrect password')
$out = Run-Repo @('ls', 'exit')
Check 'correct password unlocks' ($out -match 'Loaded repository')
Check 'ls works after unlock' ($out -match 'one\.bin')

Write-Host "== tamper detection ==" -ForegroundColor Cyan
$data = Join-Path $Work '.block\data.small'
$bytes = [IO.File]::ReadAllBytes($data)
Check 'data.small has content' ($bytes.Length -gt 4124)
if ($bytes.Length -gt 4124) {
    $off = 100   # 第 1 个 4 KiB 块属于 s4096.bin
    $bytes[$off] = $bytes[$off] -bxor 0xFF
    [IO.File]::WriteAllBytes($data, $bytes)
    $out = Run-Repo @('get s4096.bin out\tampered.bin', 'exit')
    Check 'tampered block detected' ($out -match 'authentication failed|Checksum mismatch')
    $bytes[$off] = $bytes[$off] -bxor 0xFF
    [IO.File]::WriteAllBytes($data, $bytes)
    $out = Run-Repo @('get one.bin out\one2.bin', 'exit')
    Check 'restored data readable' ((Test-Path (Join-Path $Work 'out\one2.bin')))
}

Write-Host "== 存储分层（inline / 4 KiB / 256 KiB）==" -ForegroundColor Cyan
$tier = Join-Path $Work 'tier'
New-Item -ItemType Directory -Path $tier -Force | Out-Null
foreach ($n in 1024, 1025, 262144, 262145) {
    New-Sample (Join-Path $tier "t$n.bin") $n
}
$out = Run-Repo @("add $tier\t1024.bin /t1024", "add $tier\t1025.bin /t1025",
                  "add $tier\t262144.bin /t262144", "add $tier\t262145.bin /t262145", 'exit')
Check '1 KiB inline' ($out -match 'Added /t1024 \(1024 bytes, inline\)')
Check '1025 B -> 4k' ($out -match 'Added /t1025 \(1025 bytes, 1 x4k\)')
Check '256 KiB -> 64 x4k' ($out -match 'Added /t262144 \(262144 bytes, 64 x4k\)')
Check '256 KiB + 1 -> 256k' ($out -match 'Added /t262145 \(262145 bytes, 2 x256k\)')

$out = Run-Repo @('statu', 'exit')
Check 'statu shows inline count' ($out -match 'inline\s+:\s+\d+ file')
Check 'statu shows 256 KiB blocks' ($out -match 'big blocks\s*:\s*\d+ / \d+ used \(256 KiB each\)')
Check 'statu shows file/folder counts' (($out -match 'files\s+:\s+\d+') -and ($out -match 'folders\s+:\s+\d+'))

$tierOut = Join-Path $Work 'tier-out'
New-Item -ItemType Directory -Path $tierOut -Force | Out-Null
$out = Run-Repo @("get /t262145 $tierOut", 'exit')
$bigRestored = Join-Path $tierOut 't262145'
Check 'big-block round-trip' ((Test-Path $bigRestored) -and
                              ((Get-FileHash (Join-Path $tier 't262145.bin')).Hash -eq
                               (Get-FileHash $bigRestored).Hash))

Write-Host "== 单文件多块（长链）==" -ForegroundColor Cyan
$many = Join-Path $Work 'many.bin'
New-Sample $many (5 * 1024 * 1024)
$out = Run-Repo @("add $many /many.bin", 'statu', 'exit')
Check '5 MiB -> 20 x256k' ($out -match 'Added /many\.bin \(5242880 bytes, 20 x256k\)')
Check '20 blocks counted' ($out -match 'big blocks : \d+ / \d+ used')
$manyOut = Join-Path $Work 'many-out'
New-Item -ItemType Directory -Path $manyOut -Force | Out-Null
$out = Run-Repo @("get /many.bin $manyOut", 'exit')
Check '20-block round-trip' ((Test-Path (Join-Path $manyOut 'many.bin')) -and
                             ((Get-FileHash $many).Hash -eq (Get-FileHash (Join-Path $manyOut 'many.bin')).Hash))
$out = Run-Repo @('rm /many.bin', 'exit')
Check 'long chain freed' ($out -match 'Removed /many\.bin')

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

# 4) 反向：整棵目录导出到指定目录（保留 /archive 这一层）
$restoreA = Join-Path $Work 'restoreA'
$out = Run-Repo @("get /archive $restoreA", 'exit')
Check 'export folder reports count' ($out -match 'Extracted 3 file\(s\), \d+ dir\(s\) from /archive')
Check 'exported nested file exists' (Test-Path (Join-Path $restoreA 'archive\tree\sub\deep\c.txt'))
Check 'exported content matches' ((Get-FileHash (Join-Path $tree 'sub\b.bin')).Hash -eq
                                  (Get-FileHash (Join-Path $restoreA 'archive\tree\sub\b.bin')).Hash)

# 5) 反向：不带目标 -> 使用默认导出根（环境变量 BLOCKSTORAGE_OUT）
$out = Run-Repo @('get /archive/tree/sub/deep/c.txt', 'exit')
$defaultFile = Join-Path $Work 'dropped\archive\tree\sub\deep\c.txt'
Check 'default out root used' (Test-Path $defaultFile)

# 6) out 命令：显示 / 设置（config 并入 idx）
$out = Run-Repo @('out', 'exit')
Check 'out shows env source' ($out -match 'BLOCKSTORAGE_OUT')
$cfgOut = Join-Path $Work 'cfgout'
$saved = $env:BLOCKSTORAGE_OUT
Remove-Item Env:\BLOCKSTORAGE_OUT -ErrorAction SilentlyContinue
$out = Run-Repo -NoLoad @("load -p$Pwd0", "out $cfgOut", 'out', 'exit')
Check 'out sets config' ($out -match 'Default extract root set')
Check 'config merged into idx' (-not (Test-Path (Join-Path $Work '.block\config.json')))
Check 'config source reported' ($out -match '\.block/idx')
$env:BLOCKSTORAGE_OUT = $saved

# 7) rm 目录：不带 -r 拒绝，带 -r 删除
$out = Run-Repo @('rm /archive', 'rm /archive -r', 'ls /archive -r', 'exit')
Check 'rm dir needs -r' ($out -match 'is a directory.*use rm -r')
Check 'rm -r removes subtree' ($out -match 'Removed \d+ entries under /archive')
Check 'removed dir is gone' ($out -match 'No such file or directory')

# 8) 非法内部路径
$out = Run-Repo @("add $tree\a.txt /a/../b", 'exit')
Check 'path traversal rejected' ($out -match 'Invalid internal path')

Write-Host "== 重名拦截（大小写不敏感）==" -ForegroundColor Cyan
$dup = Join-Path $Work 'dup'
New-Item -ItemType Directory -Path (Join-Path $dup 'src') -Force | Out-Null
New-Sample (Join-Path $dup 'src\x.txt') 30
New-Sample (Join-Path $dup 'src\y.txt') 40

$out = Run-Repo @("add $dup\src\x.txt /Case/File.txt",
                  "add $dup\src\x.txt /case/file.txt",
                  "add $dup\src\x.txt /CASE/FILE.TXT",
                  "add $dup\src\y.txt /Case/File.txt",
                  'ls /Case', 'exit')
Check 'first add ok' ($out -match 'Added /Case/File\.txt')
Check 'duplicate (same path, other case) blocked' ($out -match 'Duplicate path: /case/file\.txt conflicts with existing /Case/File\.txt')
Check 'duplicate (upper case) blocked' ($out -match 'conflicts with existing /Case/File\.txt')
Check 'file vs file duplicate blocked twice' (([regex]::Matches($out, 'Duplicate path:').Count) -eq 3)

$impSrc = Join-Path $Work 'impsrc'
New-Item -ItemType Directory -Path $impSrc -Force | Out-Null
New-Sample (Join-Path $impSrc 'a.txt') 20
New-Sample (Join-Path $impSrc 'b.txt') 21
$out = Run-Repo @("import $impSrc /Imp", 'exit')
Check 'first import ok' ($out -match 'Imported 2 file\(s\)')
$out = Run-Repo @("import $impSrc /Imp", 'exit')
Check 'import with conflicts aborted' ($out -match 'Import aborted, \d+ duplicate/conflicting path')
$out = Run-Repo @('ls /Imp -r', 'exit')
Check 'aborted import wrote nothing new' (([regex]::Matches($out, '/Imp/[ab]\.txt').Count) -eq 2)

$out = Run-Repo @("add $dup\src\y.txt /Case", 'exit')
Check 'adding file where dir exists blocked' ($out -match 'Duplicate path: /Case')

Write-Host "== 默认导出根 = 程序工作区 ==" -ForegroundColor Cyan
$defDir = Join-Path $Work 'defrepo'
New-Item -ItemType Directory -Path $defDir -Force | Out-Null
$saved2 = $env:BLOCKSTORAGE_OUT
Remove-Item Env:\BLOCKSTORAGE_OUT -ErrorAction SilentlyContinue
$out = Run-Repo -NoLoad -Dir $defDir @("init -p$Pwd0", 'out', 'exit')
Check 'default root is cwd\extract' ($out -match 'defrepo\\extract')
Check 'default source reported' ($out -match 'source\s*:\s*default')
$env:BLOCKSTORAGE_OUT = $saved2

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
Check 'empty dir restored' (Test-Path (Join-Path $mirrorOut 'm\emptydir'))
Check 'nested empty dir restored' (Test-Path (Join-Path $mirrorOut 'm\src\empty'))
Check 'file restored' ((Get-FileHash (Join-Path $mirror 'root.txt')).Hash -eq
                       (Get-FileHash (Join-Path $mirrorOut 'm\root.txt')).Hash)

# 目录单独导出：精确命中目录条目时也应导出整棵树
$onlyEmpty = Join-Path $Work 'only-empty'
$out = Run-Repo @("get /m/emptydir $onlyEmpty", 'exit')
Check 'exact dir export works' (Test-Path (Join-Path $onlyEmpty 'm\emptydir'))

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
$cnFile = Join-Path $cnOut '备份\中文目录\子目录\代码.cpp'
Check 'chinese round-trip' ((Test-Path $cnFile) -and
                            ((Get-Content $cnFile -Raw) -eq (Get-Content (Join-Path $cn '子目录\代码.cpp') -Raw)))

Write-Host "== brow（交互式浏览 / 快速加入 / 导出）==" -ForegroundColor Cyan
$browSrc = Join-Path $Work 'browsrc'
New-Item -ItemType Directory -Path $browSrc -Force | Out-Null
New-Sample (Join-Path $browSrc 'quick.bin') 5000
$browDir = Join-Path $Work 'browrepo'
New-Item -ItemType Directory -Path $browDir -Force | Out-Null
$browSrcRel = '..\browsrc'
# 注意：数组里不要写 'a' + $x（逗号优先级更高，会被拆成多个元素），用字符串插值
$browLines = @("init -p$Pwd0", "load -p$Pwd0", 'brow', 'pwd', "lls $browSrcRel",
               "add $browSrcRel\quick.bin", 'ls', 'cd /', 'get /quick.bin export',
               'quit', 'ls', 'exit')
$out = Run-Repo -NoLoad -Dir $browDir -Lines $browLines
Check 'brow starts' ($out -match 'BlockStorage browser')
Check 'brow prompt shows path' ($out -match 'brow:/>')
Check 'brow lists local folder' ($out -match 'TYPE  SIZE          NAME' -and $out -match 'quick\.bin')
Check 'brow quick add' ($out -match 'Added /quick\.bin')
Check 'brow export keeps structure' (Test-Path (Join-Path $browDir 'export\quick.bin'))
Check 'brow quit returns to shell' ($out -match 'BlockStorage> TYPE\s+SIZE')

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
