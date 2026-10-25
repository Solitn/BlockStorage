# BlockStorage 架构说明

> v1.0.0 · C++17 · CMake · OpenSSL 3.5 · nlohmann/json 3.12
> 本文档描述**当前实现**（含区块存储引擎），并记录运行时约束与已知限制。

## 1. 概览

BlockStorage 是一个命令行加密"块存储"工具：在当前目录创建 `.block/` 仓库，把文件切块加密写入，
靠密码派生的密钥解锁。索引与数据块都使用 AES-256-GCM，逐块独立加密。

```
CLI (REPL)  →  command  →  files (内部路径/索引/分块)  →  store (定长记录/位图)  →  .block/*
                    ↘ config (导出根)        ↘ encrypt (PBKDF2 / AES-GCM / SHA-256)
```

| 层 | 文件 | 职责 |
|---|---|---|
| 入口 | `src/main.cpp` | REPL、EOF/exit 处理、结果打印 |
| 命令 | `src/command.cpp` | 命令表、参数解析、`help <cmd>` 详情、全部命令实现 |
| 文件语义 | `src/files.cpp` | 内部路径归一化、目录树导入导出、分块与小文件分流、索引 JSON |
| 配置 | `src/config.cpp` | 导出根目录：env `BLOCKSTORAGE_OUT` > `.block/config.json` > `C:\BlockStorage\extract` |
| 存储 | `src/store.cpp` | 定长记录读写、位图分配、目录创建 |
| 密码学 | `src/encrypt.cpp` | PBKDF2、SHA-256、AES-256-GCM、Base64、子密钥派生 |

## 2. 磁盘布局

```
.block/
├── key            salt(32) ‖ SHA256(派生密钥)(32)       明文，仅用于校验密码
├── idx            AES-256-GCM 加密的索引 JSON（含内联小文件）
├── data.blk       大块区：262172 B = nonce(12) ‖ ct(256 KiB) ‖ tag(16)
├── bitmap         大块位图，1 bit/块
├── data.small     小块区：4124 B = nonce(12) ‖ ct(4 KiB) ‖ tag(16)
├── bitmap.small   小块位图
└── config.json    可选：{"output_root": "..."}（明文，非敏感）
```

**三档分流**（`Format` 结构体统一控制，旧仓库按索引版本回退到旧尺寸）：

| 文件大小 | kind | 存放 |
|---|---|---|
| ≤ 1 KiB | `inline` | base64 直接写进索引（索引本身 AES-GCM 加密） |
| 1 KiB ~ 256 KiB | `small` | 4 KiB 分块 → `data.small` |
| > 256 KiB | `blk` | 256 KiB 分块 → `data.blk` |

- 末块零填充，真实长度记在索引 `size`
- **分配**：`bitmap_first_free()` 找首个空位；位图不够长时按 `record_count()` 追加
- **回收**：删除时清位图；数据区文件不收缩（后续分配会复用空洞）
- **兼容**：索引 ≤ v3 的仓库沿用旧尺寸（data.blk 4124 B/块、data.small 540 B/块）

索引（解密后，v4 起键为内部路径且含目录条目）：

```json
{
  "version": 4,
  "files": {
    "/docs":       {"kind": "dir"},
    "/docs/empty": {"kind": "dir"},
    "/docs/t.txt": {"kind": "inline", "size": 40,    "sha256": "b64", "data": "5Lit5paH"},
    "/docs/a.bin": {"kind": "small",  "size": 100000, "sha256": "b64", "blocks": [0, 1, 2]},
    "/docs/b.bin": {"kind": "blk",    "size": 1048576,"sha256": "b64", "blocks": [0, 1, 2, 3]}
  }
}
```

### 2.1 内部路径

- 统一 `/` 分隔；输入中的 `\` 自动转换
- `NormalizeInternalPath()` 负责：折叠重复分隔符、拒绝 `.`/`..`/含 `:` 的段、去尾部斜杠
- 目录有两种来源：文件路径的每一级父目录（`ensure_parent_dirs()`），以及显式 `kind:"dir"` 条目（`import` 建立，支持空目录）
- 旧索引自动迁移：v1 的扁平键 `a.txt` → `/a.txt`；v2 无目录条目，读取时按路径推断

### 2.2 目录导入 / 导出

| 操作 | 实现 |
|---|---|
| 导入 | `add_tree()` 遍历目录：先为每个子目录建 `dir` 条目（含空目录），再逐个文件调用 `add_file()` |
| 导出 | `get_file()` 精确命中文件→写单文件；精确命中目录或前缀命中→先建目录条目再写文件 |
| 删除 | `remove_file()` 精确命中文件→删单文件；目录（自身 + 全部子孙）需 `-r`，逐个清位图后统一写索引 |
| 列表 | `list_files()` 非递归折叠为"直接子项"；递归列出全部条目（含 `dir` 行） |

### 2.3 字符编码

- 入口是 `wmain`：命令行参数 UTF-16 → UTF-8（`WideToUtf8`）
- 所有本地路径一律 `PathFromUtf8()`（UTF-8 → 宽字符）后再交给 `std::filesystem`，中文路径可用
- 控制台：`SetConsoleOutputCP/SetConsoleCP(CP_UTF8)`
- 管道输入：`NormalizeInputEncoding()` 检测非法 UTF-8 时按 ANSI（GBK）解码后转 UTF-8，
  兼容 cmd 管道与 PowerShell 管道；PowerShell 5.1 仍需设 `$OutputEncoding` 避免上游丢字

## 3. 密钥与加密

### 3.1 密码 → 密钥

```
password ‖ salt ──PBKDF2-HMAC-SHA256(600000 轮)──> derived_key(32B)
```

- `init` 无 `-s` 时用 `RAND_bytes` 生成 32 字节随机盐
- `key` 文件存 `salt ‖ SHA256(derived_key)`，`load` 时重算比对
- 派生密钥只驻留内存（`res_statu.key`），不落盘

### 3.2 每个记录的子密钥

```
subkey = SHA256(master ‖ domain ‖ block_id_be64)     domain: 1=大块, 2=小槽
```

同一主密钥下每个块使用独立子密钥与随机 nonce，避免 nonce 复用，也把密文绑定到物理位置。

### 3.3 AES-256-GCM 记录格式

```
[ nonce 12 ][ ciphertext n ][ tag 16 ]        GCM 密钥 = SHA256(key)
```

读取时 `EVP_DecryptFinal_ex` 校验 tag，失败即报 `Data authentication failed`；
索引内另有全文件 SHA-256，`get` 后再校验一次（防错位/拼接错误）。

## 4. 命令与流程

| 命令 | 实现要点 |
|---|---|
| `init [-p<pwd>] [-s<salt>]` | `create_area()` 建 6 个文件；带 `-p` 时写 `key`，并把派生密钥留在内存 |
| `load -p<pwd>` | 校验 `SHA256(DeriveKey(pwd,salt))`，成功后 `load_index()` 解密 `idx` |
| `add <src> [内路径] [-r]` | 文件→`add_file`（尾随 `/` 视为目录，取源文件名）；目录→`add_tree`（含空目录） |
| `import <目录> [内路径]` | 同 `add_tree`，但强制参数为目录；输出 `Imported N file(s), M dir(s)` |
| `get <内路径> [目标]` | 精确命中文件→写文件；目录（精确或前缀）→整棵子树，含空目录；无目标时落 `<out 根>/<内部路径>` |
| `ls [内路径] [-r]` | `list_files()`：非递归折叠为直接子项，`-r` 列全部条目（含 `dir` 行） |
| `rm <内路径> [-r]` | 精确命中文件→单文件；目录（自身 + 子孙）必须 `-r`，批量清位图后统一写索引 |
| `out [路径]` | 无参显示（含来源）；有参写入 `.block/config.json` 并建目录 |
| `statu` | 仓库标志 + 块/槽使用率 + 文件数/目录数 + 导出根 |
| `help [命令]` | 命令表；带命令名输出 `HELP_DETAILS` 中的详细说明 |
| `delete` | `remove_all(".block")` 并复位全局状态 |

**索引载入策略**：`require_loaded()` 检查 `isfind/iscomplete/hasload`，首次访问时解密 `idx`
（进程内缓存于全局 `files`，`g_index_loaded` 标记）。写入顺序为**先数据后索引**，`idx` 落盘成功才算提交。

## 5. 已知限制

- 无密码仓库使用内置固定密钥，**只混淆不保密**
- 索引整体在内存中，文件数量极大时 `save_index` 会变慢
- 数据区只追加不压缩，删除后空间靠位图复用
- 无并发控制：同一仓库被两个进程同时写入会互相覆盖
- 未做安全审计，仅供学习/自用
- 仅验证 Windows + MSVC；GCC/Clang 路径未验证

## 7. 测试

`tests/test.ps1` 覆盖 **74 项断言**：初始化、8 种尺寸的存取往返、重复添加、缺失文件、删除、
锁定/错误密码/解锁、篡改检测、无密码仓库、删除仓库；内部路径与目录树（整目录导入、显式路径、
尾斜杠入目录、目录导出、默认导出根、`out` 配置、`rm -r`、路径穿越拒绝）；`import` 含空目录镜像、
目录条目、空目录还原、精确目录导出、中文路径往返、版本横幅；以及**三档存储边界**
（1 KiB inline / 1025 B 4 KiB 块 / 256 KiB / 256 KiB+1 大块）。

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\test.ps1 -Keep
```

> 注意：测试通过管道喂命令，脚本内已设 `$OutputEncoding = UTF8`；手工写批处理时 cmd 需 `chcp 65001`。
