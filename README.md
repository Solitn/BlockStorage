# BlockStorage

把文件/目录加密后按固定大小分块存进一个 `.block/` 仓库，再按原目录结构导出。
索引是一棵 JSON 目录树（AES-256-GCM 加密），块与块之间的关系全部记录在 `bit` 链表里。

- 单一索引版本（v5），不兼容旧仓库
- 目录树存储：`ls` 可列任意目录，`-r` 递归
- 三级存储：≤1 KiB 内联、≤256 KiB 走 4 KiB 块、更大走 256 KiB 块
- 块号只出现在索引的 `head`/`tail`；文件内链、空闲链都在 `bit` 里，中间空洞可复用
- 导出始终保留仓库内层级
- 命令行提示全部为英文，路径支持中文（UTF-8）

## 快速开始

```
init -p<密码>             # 在当前目录创建 .block/（不带 -p 则只做混淆）
add <文件或目录> [路径]    # 存入仓库
ls                        # 查看
get <内部路径> [目标]      # 导出
```

示例：

```
init -pMyPass
add D:\photos /backup/photos
ls /backup -r
get /backup D:\restore     ->  D:\restore\backup\photos\...
```

## 命令一览

| 命令 | 说明 |
| --- | --- |
| `init [-p<pwd>] [-s<salt>]` | 创建仓库；`-p` 用 PBKDF2-HMAC-SHA256（600000 轮）加密 |
| `load -p<pwd>` | 解锁并解密索引（每次启动都要，免密仓库除外） |
| `add <src> [path] [-r]` | 加文件或目录；`path` 以 `/` 结尾表示放进该目录 |
| `import <dir> [path]` | 镜像目录（含空目录），与 `add <dir>` 等价 |
| `brow [path]` | 交互式浏览器：浏览 / 快速加入 / 导出 |
| `get <path> [target]` | 导出文件或整棵子树，保留原层级 |
| `ls [path] [-r]` | 列出目录；`-r` 递归 |
| `rm <path> [-r]` | 删除；目录需 `-r` |
| `out [path]` | 查看/设置默认导出根（存在索引里，没有单独配置文件） |
| `statu` | 仓库状态、块用量、文件/目录数、导出根 |
| `delete` | 删除整个 `.block/`（不可恢复） |
| `help [command]` | 命令列表 / 单条详细说明 |

内部路径统一用 `/`，例如 `/docs/2024/a.txt`。

## brow 浏览器

`brow` 进入交互模式，提示符形如 `brow:/docs>`，直到 `quit` 才回到主提示符。

| 子命令 | 作用 |
| --- | --- |
| `ls [-r]` | 列出当前仓库目录 |
| `cd <path>` / `up` | 进入目录 / 返回上级 |
| `pwd` | 当前仓库目录 |
| `lls [path]` / `lcd <path>` / `lpwd` | 浏览本地目录 / 切换 / 显示 |
| `add [本地路径] [名称]` | 快速加入：默认加到当前目录，名称缺省用文件名 |
| `get [路径] [本地目录]` | 导出，保留仓库结构 |
| `rm <名称> [-r]` | 删除 |
| `help` / `quit` | 帮助 / 退出 |

典型流程：

```
brow                    # 从根开始
lls D:\photos           # 看本地
lcd D:\photos
ls                      # 看仓库
add 2026                # 把本地 2026 整个目录加到当前仓库目录
get /docs D:\restore    # 导出
quit
```

## 导出规则（保留结构）

- 目录/子树：`get /archive D:\restore` → `D:\restore\archive\tree\...`（`/archive` 这一层也保留）
- 不带目标：写到默认导出根，路径同样是 `<根>/<完整内部路径>`
- 单个文件：
  - 目标是已存在的目录 → `<目标>/<完整内部路径>`
  - 目标是新路径 → 就当文件名，直接写这个路径
- 空目录同样会被创建出来

默认导出根的优先级：`BLOCKSTORAGE_OUT` > 索引里的 `config.output_root` > `<当前目录>\extract`。
`out <路径>` 写的是索引里的配置，因此仓库加锁时只能看到默认值。

## `.block/` 里有什么

```
.block/
├── idx          索引：AES-256-GCM 加密的 JSON（目录树 + config）
├── key          盐(32) + SHA256(派生密钥)(32)；免密仓库为空文件
├── data.blk     256 KiB 载荷的大块区
├── data.small   4 KiB 载荷的小块区
└── bit          块链表：头 + 两档节点（每节点 8 字节）
```

- 没有 `config.json`、没有位图文件，配置并入 `idx`
- 数据记录：`nonce(12) | ciphertext | tag(16)`，每块独立随机 nonce
- 子密钥 = `SHA256(主密钥 ‖ 域(1=大块,2=小块) ‖ 块号)`
- 索引 JSON：

```json
{
  "version": 5,
  "config": { "output_root": "D:\\restore" },
  "tree": {
    "kind": "dir",
    "children": {
      "docs": { "kind": "dir", "children": {
        "a.txt": { "kind": "inline", "size": 7, "sha256": "...", "data": "aGVsbG8=" }
      }},
      "big.bin": { "kind": "blk", "size": 300000, "sha256": "...", "head": 0, "tail": 1 }
    }
  }
}
```

### `bit` 链表

文件头 96 字节：magic、版本、节点大小、两档的容量 / 空闲数 / 链首 / 链尾；
之后是大块节点区、小块节点区，节点按块号下标随机读写。

每个节点只有 8 字节的 `next`：

- 块被占用时，`next` = 同一文件的下一个块（`-1` 表示文件末尾）
- 块空闲时，`next` = 空闲链的下一个块（`-1` 表示链尾）

分配从空闲链首取、释放追加到链尾，所以删除后留下的中间空洞会被直接复用，不要求连续。
索引里只存每文件的 `head` / `tail`，不再存块号数组。

### 分级

| 大小 | 存放 |
| --- | --- |
| ≤ 1 KiB | 直接 base64 进索引（`kind: "inline"`） |
| 1 KiB ~ 256 KiB | `data.small`，4 KiB 一块（`kind: "small"`） |
| > 256 KiB | `data.blk`，256 KiB 一块（`kind: "blk"`） |

## 约定与限制

- 重名/冲突大小写不敏感：`Duplicate path: X conflicts with existing Y`
- 目录可合并（同名目录视为同一个，沿用已有大小写）
- 导入整批预检，任何冲突会整批中止，不写入半截
- 索引版本只有 v5，旧格式仓库需要重新 `init`
- 仓库加锁时只有 `init` / `load` / `help` / `statu`（不含统计）/ `delete` 可用
- 中文路径、中文内容全程 UTF-8；`rm` 目录需要 `-r`

## 构建与测试（可选）

```
scripts\build.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\test.ps1
```
