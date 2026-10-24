# BlockStorage 端到端测试
# 用法： pwsh -ExecutionPolicy Bypass -File .\tests\test.ps1 [-Exe <path>] [-Keep]
param(
    [string]$Exe = '',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
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
foreach ($k in $sizes.Keys) { Check "add $k" ($out -match [regex]::Escape("Added $k")) }
$lsRows = ($out -split "`n") | Where-Object { $_ -match '^\S+\.bin\s+\d+' }
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
Check 'rm works' ($out -match 'Removed big.bin')
$lsRows = ($out -split "`n") | Where-Object { $_ -match '^\S+\.bin\s+\d+' }
Check 'ls no longer lists big.bin' (-not ($lsRows -match '^big\.bin'))

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

Write-Host "== no-password repository ==" -ForegroundColor Cyan
$plain = Join-Path $Work 'plain'
New-Item -ItemType Directory -Path $plain -Force | Out-Null
New-Sample (Join-Path $plain 'a.bin') 100
$out = Run-Repo -NoLoad -Dir $plain @('init', 'add a.bin', 'get a.bin a.out', 'exit')
Check 'keyless init ok' ($out -match 'no password')
Check 'keyless add ok' ($out -match 'Added a.bin')
Check 'keyless round-trip' ((Get-FileHash (Join-Path $plain 'a.bin')).Hash -eq (Get-FileHash (Join-Path $plain 'a.out')).Hash)

Write-Host "== delete repository ==" -ForegroundColor Cyan
$out = Run-Repo @('delete', 'statu', 'exit')
Check 'delete works' ($out -match 'Repository deleted')
Check '.block removed' (-not (Test-Path (Join-Path $Work '.block')))

Write-Host ""
Write-Host ("PASS=$pass FAIL=$fail") -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if (-not $Keep -and (Test-Path $Work)) { Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue }
if ($fail -gt 0) { exit 1 }
