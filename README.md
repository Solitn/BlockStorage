# BlockStorage

> 当前版本 **1.0.0**

一个用 C++17 写的**命令行加密块存储仓库**：把文件切块写入本地仓库目录 `.block/`，
索引与数据块都以 AES-256-GCM 加密，用密码派生密钥解锁。

```
BlockStorage v1.0.0
BlockStorage, made by @solitn
Github: https://github.com/solitn/blockstorage
Repository not found
BlockStorage> init -pMyPassword
...
BlockStorage> add D:\docs\report.pdf report.pdf
Added report.pdf (245760 bytes, 60 blocks)
BlockStorage> ls
NAME                             SIZE      KIND
report.pdf                       245760    blk
BlockStorage> get report.pdf D:\out\report.pdf
Extracted report.pdf -> D:\out\report.pdf (245760 bytes)
```

## 功能

- **分块存储**：4 KiB 定长大块（`data.blk` + `bitmap`），小文件（≤512 B）走 `data.small` 定长槽，减少碎片
- **加密**：索引与每个数据块都用 AES-256-GCM 独立加密，块头带随机 nonce，`tag` 校验失败即判定篡改
- **完整性**：索引记录每个文件的 SHA-256，取回时校验
- **子密钥隔离**：每个块用 `SHA256(master ‖ domain ‖ block_id)` 派生的独立子密钥
- **密码派生**：PBKDF2-HMAC-SHA256，60 万次迭代，随机盐随 `.block/key` 保存

## 运行

直接用已编译好的 `build\BlockStorage.exe`（同目录已带 `libcrypto-3-x64.dll`），
在**目标目录**启动即可，仓库会建在当前目录的 `.block/`：

```bat
cd /d D:\vault
C:\path\to\blockstorage\build\BlockStorage.exe
```

需要重新编译时执行 `scripts\build.cmd`；测试执行 `powershell -ExecutionPolicy Bypass -File .\tests\test.ps1`。

## 命令

> 完整操作手册（含典型工作流、错误速查、FAQ）：[help.md](help.md)

| 命令 | 说明 |
|---|---|
| `init [-p<pwd>] [-s<salt>]` | 初始化仓库；带 `-p` 用密码加密，`-s` 指定盐（默认随机） |
| `load -p<pwd>` | 解锁仓库（每个进程启动后都要解锁） |
| `add <源> [内路径] [-r]` | 加入文件或整个文件夹（保留子目录结构） |
| `import <目录> [内路径]` | 完整镜像目录：所有文件 + 所有子目录（**含空目录**） |
| `get <内路径> [目标]` | 取出文件或整个目录树（含空目录）；省略目标则用默认导出根 |
| `ls [内路径] [-r]` | 列出条目（`-r` 递归，目录带 `/` 后缀） |
| `rm <内路径> [-r]` | 删除文件；目录需 `-r` |
| `out [路径]` | 查看/设置默认导出根目录 |
| `statu` | 仓库状态、块与槽使用率、文件/目录数、导出根 |
| `delete` | 删除整个 `.block/` 仓库 |
| `help [命令]` | 帮助（带命令名显示详细用法） |
| `exit` / `quit` | 退出 |

### 内部路径

仓库内统一用 `/` 寻址，例如 `/docs/report.pdf`。目录既可由文件路径隐式形成，
也可由 `import` 显式建条目（因此空目录也能保存/还原）：

```text
import D:\photos /backup/photos  -> /backup/photos/...（含空目录）
add D:\photos                    -> /photos/...
add D:\a.txt /docs/              -> /docs/a.txt
get /backup/photos D:\restore    -> D:\restore\...
```

路径支持中文（内部统一 UTF-8）。

### 重名规则（大小写不敏感）

- 同一路径只能存在一个条目：`/Case/File.txt` 与 `/case/file.txt`、`/CASE/FILE.TXT` 视为同一个名字，重复写入会被拒绝
- 目标路径已被**目录**占用时，不能再当文件写入；反之亦然
- 父目录已存在但大小写不同时，会自动沿用仓库里的写法（`/Docs` 下再写 `/docs/a.txt` → 落到 `/Docs/a.txt`）
- `import` 采用**整批拦截**：预扫描发现任何冲突就整体中止，一条都不写（错误信息会列出前 3 处冲突）

### 默认导出根目录

`get` 不带目标时写入 `<out 根>/<内部路径>`，优先级：

1. 环境变量 `BLOCKSTORAGE_OUT`
2. `.block/config.json` 的 `output_root`（由 `out <路径>` 写入）
3. 内置默认 `<程序工作目录>\extract`（即仓库所在目录下的 `extract`）

`scripts/env.ps1` 会把 `BLOCKSTORAGE_OUT` 指向仓库内 `out\extract`，方便开发测试。

### 编码说明

- 程序入口是 `wmain`，命令行参数按 UTF-16 接收后转 UTF-8，中文路径/文件名可直接使用
- 控制台输出统一 UTF-8；交互输入会自动识别 UTF-8 或 GBK
- **管道/批处理**：cmd 里请先 `chcp 65001`；PowerShell 里设 `$OutputEncoding = [Text.Encoding]::UTF8`
  （Windows PowerShell 5.1 默认 ASCII，中文会变成 `?`）

## 仓库格式

```
.block/
├── key           salt(32) ‖ SHA256(派生密钥)(32)   ← 明文，用于校验密码
├── idx           AES-256-GCM 加密的 JSON 索引（含内联小文件）
├── data.blk      大块区：nonce(12) ‖ ct(256 KiB) ‖ tag(16) = 262172 B/块
├── bitmap        大块分配位图（1 bit/块）
├── data.small    小块区：nonce(12) ‖ ct(4 KiB) ‖ tag(16) = 4124 B/块
└── bitmap.small  小块分配位图
```

**三档存储**（按文件大小自动选择）：

| 文件大小 | 存法 |
|---|---|
| ≤ 1 KiB | **内联**：内容 base64 直接写进加密索引，不占数据区 |
| 1 KiB ~ 256 KiB | **小块**：按 4 KiB 分块写入 `data.small` |
| > 256 KiB | **大块**：按 256 KiB 分块写入 `data.blk` |

索引 JSON（解密后）：

```json
{
  "version": 4,
  "files": {
    "/docs":            {"kind": "dir"},
    "/docs/empty":      {"kind": "dir"},
    "/docs/note.txt":   {"kind": "inline", "size": 42,      "sha256": "…", "data": "5Lit5paH"},
    "/docs/small.bin":  {"kind": "small",  "size": 100000,  "sha256": "…", "blocks": [0,1,2]},
    "/docs/report.pdf": {"kind": "blk",    "size": 1048576, "sha256": "…", "blocks": [0,1,2,3]}
  }
}
```

旧仓库（索引版本 ≤3）仍可读写：记录尺寸按索引版本自动切换。

## 安全说明

- **无密码仓库**（`init` 不带 `-p`）使用代码内置的固定密钥，只提供混淆，**不提供机密性**，请勿存放敏感数据。
- 密码只在 `load` 时使用；派生密钥仅驻留内存，不落盘（`.block/key` 只存盐与 SHA-256 摘要）。
- 仓库目录 `.block/` 已加入 `.gitignore`，切勿提交。
- 这是学习/自用性质的项目，未经安全审计。

## 目录结构

```
.
├── CMakeLists.txt
├── README.md              快速开始与命令参考
├── help.md                完整操作指南（工作流 / 错误速查 / FAQ）
├── ARCHITECTURE.md        设计说明与已知限制
├── include/               头文件（含 nlohmann/json 单头文件）
├── src/                   实现
│   ├── main.cpp           wmain 入口（UTF-16 → UTF-8）+ REPL
│   ├── command.cpp        命令表与各命令实现
│   ├── files.cpp          内部路径 / 索引 / 分块读写 / 目录导入导出
│   ├── store.cpp          定长记录 / 位图 / 目录管理
│   ├── config.cpp         导出根目录配置（env / config.json / 默认）
│   ├── utf8.cpp           UTF-8 ↔ 宽字符、管道输入编码归一化
│   └── encrypt.cpp        PBKDF2 / AES-GCM / SHA-256 / Base64
├── scripts/               便携工具链下载、环境、构建脚本
├── tests/test.ps1         端到端测试（67 项断言）
└── tools/                 便携工具链（已忽略，不入库）
```
