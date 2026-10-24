# BlockStorage

一个用 C++17 写的**命令行加密块存储仓库**：把文件切块写入本地仓库目录 `.block/`，
索引与数据块都以 AES-256-GCM 加密，用密码派生密钥解锁。

```
BlockStorage v0.1
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

## 构建

依赖：CMake ≥ 3.16、C++17 编译器（MSVC 或 GCC）、OpenSSL 3.x。

### Windows（本机已配好的便携工具链）

```powershell
# 一键构建（内部自动 dot-source env.ps1、生成缺失的 oldnames.lib）
scripts\build.cmd            # 或： scripts\build.cmd -Clean
```

### 从零准备工具链（无需管理员、不污染系统）

```powershell
# 1) OpenSSL + CMake
python scripts\setup_tools.py openssl cmake

# 2) 便携 MSVC（从 VS 2022 通道解包）+ /MT 所需的 CRT 资源
python scripts\fetch_msvc.py msvc

# 3) Windows SDK 头文件与库（NuGet 包，约 210 MB，慢网可反复续传）
python scripts\fetch_url.py https://globalcdn.nuget.org/packages/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg        tools\downloads\microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg 150000000
python scripts\fetch_url.py https://globalcdn.nuget.org/packages/microsoft.windows.sdk.cpp.x64.10.0.28000.2705.nupkg    tools\downloads\microsoft.windows.sdk.cpp.x64.10.0.28000.2705.nupkg 50000000
python scripts\fetch_url.py https://globalcdn.nuget.org/packages/microsoft.windows.sdk.buildtools.10.0.26100.1742.nupkg tools\downloads\sdktools.nupkg 20000000

# 4) 解包 SDK（含 rc.exe / mt.exe）
python scripts\unpack_sdk.py

# 5) 构建
scripts\build.cmd
```

> 说明：便携 MSVC 包缺少 `msvcprt.lib` 等导入库，因此构建使用**静态 CRT（/MT）**；
> 可执行文件只依赖 `libcrypto-3-x64.dll`（构建后自动复制到输出目录）。

### 已有系统工具链

```bash
export OPENSSL_ROOT_DIR=/path/to/openssl
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 测试

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\test.ps1 -Keep     # -Keep 保留测试目录
```

## 命令

> 完整操作手册（含典型工作流、错误速查、FAQ）：[help.md](help.md)

| 命令 | 说明 |
|---|---|
| `init [-p<pwd>] [-s<salt>]` | 初始化仓库；带 `-p` 用密码加密，`-s` 指定盐（默认随机） |
| `load -p<pwd>` | 解锁仓库（每个进程启动后都要解锁） |
| `add <源> [内路径] [-r]` | 加入文件或整个文件夹（保留子目录结构） |
| `get <内路径> [目标]` | 取出文件或整个目录树；省略目标则用默认导出根 |
| `ls [内路径] [-r]` | 列出条目（`-r` 递归） |
| `rm <内路径> [-r]` | 删除文件；目录需 `-r` |
| `out [路径]` | 查看/设置默认导出根目录 |
| `statu` | 仓库状态、块与槽使用率、文件/目录数、导出根 |
| `delete` | 删除整个 `.block/` 仓库 |
| `help [命令]` | 帮助（带命令名显示详细用法） |
| `exit` / `quit` | 退出 |

### 内部路径

仓库内统一用 `/` 寻址，例如 `/docs/report.pdf`。目录由文件路径隐式形成：

```text
add D:\photos                 -> /photos/2024/a.jpg
add D:\photos /backup/photos  -> /backup/photos/2024/a.jpg
add D:\a.txt /docs/           -> /docs/a.txt
get /backup/photos D:\restore -> D:\restore\2024\a.jpg
```

### 默认导出根目录

`get` 不带目标时写入 `<out 根>/<内部路径>`，优先级：

1. 环境变量 `BLOCKSTORAGE_OUT`
2. `.block/config.json` 的 `output_root`（由 `out <路径>` 写入）
3. 内置默认 `C:\BlockStorage\extract`

`scripts/env.ps1` 会把 `BLOCKSTORAGE_OUT` 指向仓库内 `out\extract`，方便开发测试。

## 仓库格式

```
.block/
├── key           salt(32) ‖ SHA256(派生密钥)(32)   ← 明文，用于校验密码
├── idx           AES-256-GCM 加密的 JSON 索引
├── data.blk      大块区：每块 nonce(12) ‖ ct(4096) ‖ tag(16) = 4124 B
├── bitmap        大块分配位图（1 bit/块）
├── data.small    小文件区：每槽 nonce(12) ‖ ct(512) ‖ tag(16) = 540 B
└── bitmap.small  小槽分配位图
```

索引 JSON（解密后，v2 起使用内部路径）：

```json
{
  "version": 2,
  "files": {
    "/docs/report.pdf": {"kind": "blk",   "size": 245760, "sha256": "…", "blocks": [0, 1, 2]},
    "/docs/note.txt":   {"kind": "small", "size": 42,     "sha256": "…", "slot": 7}
  }
}
```

旧的扁平索引（v1，键为 `a.txt`）在 `load` 时会自动迁移为 `/a.txt`。

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
│   ├── main.cpp           REPL 入口
│   ├── command.cpp        命令表与各命令实现
│   ├── files.cpp          内部路径 / 索引 / 分块读写 / 目录导入导出
│   ├── store.cpp          定长记录 / 位图 / 目录管理
│   ├── config.cpp         导出根目录配置（env / config.json / 默认）
│   └── encrypt.cpp        PBKDF2 / AES-GCM / SHA-256 / Base64
├── scripts/               便携工具链下载、环境、构建脚本
├── tests/test.ps1         端到端测试（53 项断言）
└── tools/                 便携工具链（已忽略，不入库）
```
