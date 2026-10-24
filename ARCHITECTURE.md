# BlockStorage 架构说明

> v0.1 · C++17 · CMake · OpenSSL 3.5 · nlohmann/json 3.12
> 本文档描述**当前实现**（含区块存储引擎），并记录运行时约束与已知限制。

## 1. 概览

BlockStorage 是一个命令行加密"块存储"工具：在当前目录创建 `.block/` 仓库，把文件切块加密写入，
靠密码派生的密钥解锁。索引与数据块都使用 AES-256-GCM，逐块独立加密。

```
CLI (REPL)  →  command  →  files (索引/文件语义)  →  store (定长记录/位图)  →  .block/*
                                   ↘ encrypt (PBKDF2 / AES-GCM / SHA-256)
```

| 层 | 文件 | 职责 |
|---|---|---|
| 入口 | `src/main.cpp` | REPL、EOF/exit 处理、结果打印 |
| 命令 | `src/command.cpp` | 命令表、参数解析、`init/load/delete/statu/help` |
| 文件语义 | `src/files.cpp` | 增删查取、分块与小文件分流、索引 JSON 读写 |
| 存储 | `src/store.cpp` | 定长记录读写、位图分配、目录创建 |
| 密码学 | `src/encrypt.cpp` | PBKDF2、SHA-256、AES-256-GCM、Base64、子密钥派生 |

## 2. 磁盘布局

```
.block/
├── key            salt(32) ‖ SHA256(派生密钥)(32)       明文，仅用于校验密码
├── idx            AES-256-GCM 加密的索引 JSON
├── data.blk       大块区：每块 4124 B = nonce(12) ‖ ct(4096) ‖ tag(16)
├── bitmap         大块位图，1 bit/块
├── data.small     小槽区：每槽 540 B = nonce(12) ‖ ct(512) ‖ tag(16)
└── bitmap.small   小槽位图
```

- **分流阈值**：`size ≤ 512` 走小槽，否则按 4096 B 分块（末块零填充，真实长度记在索引里）
- **分配**：`bitmap_first_free()` 找首个空位；位图不够长时按 `record_count()` 追加
- **回收**：删除时清位图；数据区文件不收缩（后续分配会复用空洞）

索引（解密后）：

```json
{
  "version": 1,
  "files": {
    "a.bin": {"kind": "blk",   "size": 5000, "sha256": "b64", "blocks": [0, 3]},
    "b.txt": {"kind": "small", "size": 40,   "sha256": "b64", "slot": 2}
  }
}
```

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
| `add <src> [name]` | 读文件 → 分流 → 逐块加密写盘 → 更新位图 → `save_index()` |
| `get <name> [dst]` | 读记录 → 解密 → 截断到 `size` → 校验 SHA-256 → 写目标 |
| `ls` | 遍历索引，按名字排序，标注 `small`/`blk` |
| `rm <name>` | 清位图 → 从索引删除 → `save_index()` |
| `statu` | 仓库标志 + 块/槽使用率 + 文件数 |
| `delete` | `remove_all(".block")` 并复位全局状态 |

**索引载入策略**：`require_loaded()` 检查 `isfind/iscomplete/hasload`，首次访问时解密 `idx`
（进程内缓存于全局 `files`，`g_index_loaded` 标记）。写入顺序为**先数据后索引**，`idx` 落盘成功才算提交。

## 5. 运行时约束（重要）

便携工具链（`tools/`，由 `scripts/` 下载，已 gitignore）有几个非标准点：

1. **静态 CRT（/MT）**：从 VS 通道解包的 MSVC 包缺少 `msvcprt.lib` / `vcruntime.lib` 等 DLL 导入库，
   因此 `CMakeLists.txt` 强制 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`，并通过
   `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES` 传给编译器探测阶段（否则 try_compile 用 `/MDd` 失败）。
2. **oldnames.lib**：Windows SDK 的 NuGet 包不含该库，`scripts/build.ps1` 用 `lib.exe /def:` 生成空壳。
3. **rc.exe / mt.exe**：清单嵌入需要资源编译器，来自 `microsoft.windows.sdk.buildtools` 包，放在 `tools/sdk/bin`。
4. **执行策略**：脚本用 `powershell -ExecutionPolicy Bypass -File`（或 `scripts\build.cmd`）运行。
5. **运行时依赖**：可执行文件静态链接 CRT，仅需 `libcrypto-3-x64.dll`（构建后自动复制到输出目录）。

## 6. 已知限制

- 无密码仓库使用内置固定密钥，**只混淆不保密**
- 索引整体在内存中，文件数量极大时 `save_index` 会变慢
- 数据区只追加不压缩，删除后空间靠位图复用
- 无并发控制：同一仓库被两个进程同时写入会互相覆盖
- 未做安全审计，仅供学习/自用
- 仅验证 Windows + MSVC；GCC/Clang 路径未验证

## 7. 测试

`tests/test.ps1` 覆盖 38 项断言：初始化、8 种尺寸的存取往返（0/1/511/512/513/4096/4097/1 MiB）、
重复添加、缺失文件、删除、锁定/错误密码/解锁、篡改检测、无密码仓库、删除仓库。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
powershell -ExecutionPolicy Bypass -File .\tests\test.ps1 -Keep
```
