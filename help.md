# BlockStorage 操作指南

面向使用者的完整操作手册。设计/实现细节见 [ARCHITECTURE.md](ARCHITECTURE.md)，构建细节见 [README.md](README.md)。

---

## 1. 三分钟上手

```text
D:\work> BlockStorage.exe
BlockStorage v1.0.0
Repository not found
BlockStorage> init -pMyPass123
...
BlockStorage> add D:\docs\report.pdf /docs/report.pdf
Added /docs/report.pdf (245760 bytes, 60 blocks)
BlockStorage> import D:\photos /backup/photos
Imported 128 file(s), 12 dir(s) into /backup/photos
BlockStorage> ls /
dir   -             /backup/
dir   -             /docs/
BlockStorage> get /docs/report.pdf D:\restore\report.pdf
Extracted /docs/report.pdf -> D:\restore\report.pdf (245760 bytes)
BlockStorage> exit
```

> 仓库位于**当前工作目录**的 `.block/` 下。换目录启动就是另一个仓库。

---

## 2. 核心概念

### 2.1 仓库（`.block/`）

`init` 在当前目录创建：

| 文件 | 作用 |
|---|---|
| `key` | 盐 + 密码校验摘要（明文，不含密码本身） |
| `idx` | 加密的文件索引（哪些文件、多大、在哪几块） |
| `data.blk` / `bitmap` | 大块数据区与分配位图 |
| `data.small` / `bitmap.small` | 小文件（≤512 B）内联区与位图 |
| `config.json` | 可选，记录默认导出根目录 |

### 2.2 内部路径

仓库内所有文件用**统一的正斜杠路径**寻址：

- `/` 是仓库根
- `/docs/report.pdf`、`/backup/photos/2024/a.jpg`
- 目录由两层构成：文件路径的每一级父目录自动存在；`import` 还会为每个目录（**含空目录**）建立显式目录条目
- 反斜杠会自动转换；`.`、`..`、盘符（`C:`）等非法段会被拒绝
- 路径按 UTF-8 存储，中文目录名可直接使用

### 2.3 默认导出根目录（out）

`get` 不带目标路径时，文件会落到：

```
<out 根>/<内部路径>
```

`<out 根>` 的优先级（高 → 低）：

1. 环境变量 `BLOCKSTORAGE_OUT`
2. 仓库内 `.block/config.json` 的 `output_root`
3. 内置默认值 `C:\BlockStorage\extract`

---

## 3. 命令详解

### 3.1 `init` — 建仓库

```text
init                         # 无密码：仅混淆，不保密
init -pMyPass123             # 用密码加密（随机盐）
init -pMyPass123 -sMySalt    # 指定盐（一般不需要）
```

- 密码经 PBKDF2-HMAC-SHA256（60 万轮）派生 32 字节密钥
- 每次 `init` 会打印盐与派生密钥的 Base64；**密码丢失无法恢复数据**
- 已存在 `.block/` 时会拒绝，需先 `delete`

### 3.2 `load` — 解锁

```text
load -pMyPass123
```

- 每个新进程都要解锁一次（无密码仓库自动解锁）
- 密码错误：`Error: Incorrect password`
- 未解锁就操作：`Error: Repository is locked, run 'load -p<password>'`

### 3.3 `add` — 加入文件 / 文件夹

```text
add <源路径> [内部路径] [-r]
```

| 场景 | 命令 | 结果 |
|---|---|---|
| 单文件，用原名 | `add D:\a.txt` | `/a.txt` |
| 单文件，改名 | `add D:\a.txt /docs/report.txt` | `/docs/report.txt` |
| 单文件，放进目录 | `add D:\a.txt /docs/` | `/docs/a.txt`（尾随斜杠 = 目录） |
| 整个文件夹 | `add D:\proj` | `/proj/...`（保留子目录） |
| 文件夹 → 指定内部路径 | `add D:\proj /backup/2026/proj` | `/backup/2026/proj/...` |
| 显式递归（等价） | `add D:\proj /backup -r` | `/backup/proj/...` |

要点：

- 文件夹会**递归**导入其下所有文件与子目录，**空目录也会保留**（v1.0.0 起）
- 同名内部路径已存在时整体拒绝：`Error: Path already exists in repository: /x/y`
- 文件夹导入会输出 `Imported N file(s), M dir(s) into <路径>`，失败项计入 `skipped`
- 单文件 ≤512 B 走小槽（省空间），更大文件按 4 KiB 分块

### 3.4 `import` — 完整镜像一个目录

```text
import <本地目录> [内部路径]
```

把一个本地目录**完整**写进仓库：其下所有文件 + 所有子目录（含空目录）都会建立条目。

| 场景 | 命令 | 结果 |
|---|---|---|
| 镜像目录，用目录名 | `import D:\proj` | `/proj/...`（含空目录） |
| 镜像到指定内部路径 | `import D:\proj /backup/proj-1.0` | `/backup/proj-1.0/...` |
| 只导入空目录 | `import D:\templates /tpl` | `/tpl`（0 文件，目录条目保留） |

- 与 `add <目录>` 行为一致；区别只在语义：`import` 明确要求参数是目录，单文件请用 `add`
- 输出形如 `Imported 128 file(s), 20 dir(s) into /backup/proj`
- 已存在的路径会被跳过（计入 `skipped`），不会覆盖
- 反向导出用 `get`，空目录会一并还原

### 3.5 `get` — 取出（反向）

```text
get <内部路径> [目标]
```

| 场景 | 命令 | 结果 |
|---|---|---|
| 取单个文件到指定路径 | `get /docs/a.txt D:\out\a.txt` | 写入该文件 |
| 取单个文件到目录 | `get /docs/a.txt D:\out` | `D:\out\a.txt` |
| 取单个文件用默认根 | `get /docs/a.txt` | `<out 根>\docs\a.txt` |
| 取整个目录 | `get /docs D:\restore` | `D:\restore\<docs 下的相对路径>` |
| 取整个目录用默认根 | `get /docs` | `<out 根>\docs\...` |
| 取整个仓库 | `get /` | `<out 根>\...` |

- 目录导出会保留完整目录结构（含空目录），自动创建所需文件夹
- 输出形如 `Extracted 128 file(s), 20 dir(s) from /docs -> D:\restore`
- 每个文件取回时都会校验 SHA-256，不匹配会报 `Checksum mismatch`
- 数据块被篡改时 `GCM` tag 校验失败：`Data authentication failed`

### 3.6 `ls` — 查看

```text
ls                  # 根目录直接子项（目录带 / 后缀）
ls /docs            # 指定目录的直接子项
ls /docs -r         # 递归列出所有条目（含目录）
```

输出示例：

```text
TYPE  SIZE          PATH
dir   -             /docs/
dir   -             /docs/empty/
file  245760        /docs/report.pdf
```

### 3.7 `rm` — 删除

```text
rm /docs/a.txt      # 删除单个文件
rm /docs -r         # 递归删除整个目录（含目录条目）
```

- 删除目录必须带 `-r`，否则提示 `is a directory (N entries), use rm -r`
- 删除会回收块/槽位图并重写索引，数据块空间由后续写入复用

### 3.8 `out` — 默认导出根目录

```text
out                       # 查看当前值与来源（env / config / default）
out D:\restore            # 设置并写入 .block/config.json（目录会被创建）
```

### 3.9 其他

```text
statu        # isfind/iscomplete/haskey/hasload、块使用率、文件数与目录数、out 根
delete       # 删除整个 .block/（不可恢复）
help         # 命令列表
help add     # 单条命令的详细说明（import / get / ls / rm / out 同样支持）
exit / quit  # 退出
```

### 3.10 中文路径与编码

- 命令行参数按 UTF-16 → UTF-8 处理，**中文路径/文件名直接可用**
- 交互输入会自动识别 UTF-8 或 GBK
- 管道/批处理：cmd 先 `chcp 65001`；PowerShell 设 `$OutputEncoding = [Text.Encoding]::UTF8`
  （Windows PowerShell 5.1 默认 ASCII，中文会变成 `?`）

---

## 4. 典型工作流

### 4.1 备份一个项目目录并还原

```text
init -pBackup2026
add D:\projects\app /backups/app-2026-05
exit

:: 之后任意时候
BlockStorage.exe
load -pBackup2026
get /backups/app-2026-05 D:\restore
exit
```

### 4.2 用批处理脚本无交互执行

```bat
@echo off
rem 一次性导入并导出（仓库在 D:\vault）
cd /d D:\vault
( echo load -pBackup2026
  echo add D:\data\logs /logs
  echo get /logs D:\restore\logs
  echo exit ) | BlockStorage.exe
```

### 4.3 换导出目录（不污染系统盘）

```text
out E:\vault-restore
get /docs
```

或临时用环境变量（优先级最高）：

```bat
set BLOCKSTORAGE_OUT=E:\vault-restore
BlockStorage.exe
```

---

## 5. 错误速查

| 提示 | 含义 / 处理 |
|---|---|
| `Repository not found, run 'init' first` | 当前目录没有 `.block/` |
| `Repository is locked, run 'load -p<password>'` | 先解锁 |
| `Incorrect password` | 密码错；无密码仓库不适用 |
| `Repository incomplete: some files are missing` | `.block/` 下 6 个核心文件缺失 |
| `Path already exists in repository: /x` | 内部路径重名，换个名字或先 `rm` |
| `Invalid internal path` | 含 `..`、盘符或空段 |
| `'...' is a directory (N entries), use rm -r` | 删目录要 `-r` |
| `No such file or directory in repository: /x` | 不存在（用 `ls / -r` 确认拼写） |
| `Not a directory: <路径>（单文件请用 add）` | `import` 只接受目录 |
| `数据变问号（?）` | 管道编码问题：cmd 用 `chcp 65001`，PowerShell 设 `$OutputEncoding` |
| `Data authentication failed` | 块密文被改动或密钥不符 |
| `Checksum mismatch` | 还原后 SHA-256 不符（数据损坏） |
| `Cannot write <路径>` | 目标不可写（权限/只读盘/被占用） |

---

## 6. 安全提示

- **无密码仓库只混淆不保密**：`init` 不带 `-p` 时使用内置固定密钥，任何人都能解开
- 密码遗失 = 数据无法恢复，`key` 文件里没有可逆信息
- `.block/` 目录按需备份，但**不要提交到 git**（本项目已 gitignore）
- 程序不做并发控制：同一仓库不要被多个进程同时写入
- 未经过安全审计，请勿用于生产环境的敏感数据

---

## 7. 常见问题

**Q：能不能保留空目录？**
可以（v1.0.0 起）。`import` 会为每个子目录建立 `kind:"dir"` 条目，`get` 时原样还原；
`ls` 里目录行形如 `dir   -   /docs/empty/`。

**Q：`add` 和 `import` 有什么区别？**
行为一致（都会保留空目录）。`add` 还能接受单个文件，`import` 只接受目录，语义更明确。

**Q：单个文件最大能多大？**
受限于磁盘与 `uint64` 分块计数；1 GiB 约需 262144 个块，实测按 4 KiB 分块顺序写入即可。

**Q：删除文件后仓库会变小吗？**
不会立即变小，位图释放后由后续写入复用；需要缩容只能重新 `init` 并重新导入。

**Q：能在 U 盘上用吗？**
可以，仓库就是普通文件；但写入过程中断电可能损坏索引（建议先备份 `.block/`）。

**Q：为什么每次启动都要 load？**
密钥只在内存中保存，不落盘，这是有意的设计。

**Q：中文路径/文件名可以吗？**
可以。参数走 `wmain`（UTF-16→UTF-8），内部路径按 UTF-8 存储，控制台输出也是 UTF-8。
注意管道输入：cmd 用 `chcp 65001`，PowerShell 设 `$OutputEncoding = [Text.Encoding]::UTF8`。

**Q：版本号在哪里改？**
`include/version.h`（`BS_VERSION`）与 `CMakeLists.txt` 的 `project(... VERSION 1.0.0)`。
