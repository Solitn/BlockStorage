#include "command.h"

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "encrypt.h"
#include "files.h"
#include "main.h"
#include "store.h"
#include "types.h"
#include "utf8.h"
#include "version.h"

namespace fs = std::filesystem;

const std::string VER = BS_VERSION;

namespace {

// 索引是否已解密进内存
bool g_index_loaded = false;

// 检查仓库可用并确保索引已载入
Result require_loaded() {
    getStatu({});
    if (!res_statu.isfind) return {false, "Repository not found, run 'init' first"};
    if (!res_statu.iscomplete) return {false, "Repository incomplete: some files are missing"};
    if (!res_statu.hasload) return {false, "Repository is locked, run 'load -p<password>'"};
    if (!g_index_loaded) {
        if (!load_index(res_statu.key)) {
            return {false, "Cannot load repository index (wrong key or corrupted data)"};
        }
        g_index_loaded = true;
    }
    return {true, ""};
}

}  // namespace

const std::map<std::string, CommandEntry> COMMANDS = {
    {"init",   {cmd_init,   "init [-p<pwd>] [-s<salt>]        create a repository (optionally encrypted)"}},
    {"load",   {cmd_load,   "load -p<pwd>                     unlock the repository"}},
    {"add",    {cmd_add,    "add <源> [内路径] [-r]            add a file, or a folder (keeps subdirs)"}},
    {"import", {cmd_import, "import <目录> [内路径]            mirror a folder: all files + dirs (incl. empty)"}},
    {"get",    {cmd_get,    "get <内路径> [目标]               extract a file, a folder or the whole repo"}},
    {"ls",     {cmd_ls,     "ls [内路径] [-r]                  list entries (tree with -r)"}},
    {"rm",     {cmd_rm,     "rm <内路径> [-r]                  remove a file, or a folder with -r"}},
    {"out",    {cmd_out,    "out [路径]                        show / set default extract root"}},
    {"statu",  {cmd_statu,  "statu                            show repository status"}},
    {"delete", {cmd_delete, "delete                           delete the whole repository"}},
    {"help",   {cmd_help,   "help [命令]                       show help (per-command details)"}}
};

// help <命令> 的详细说明
const std::map<std::string, std::string> HELP_DETAILS = {
    {"init",
     "init [-p<pwd>] [-s<salt>]\n"
     "  在当前目录创建 .block/ 仓库。\n"
     "  -p<pwd>   用密码加密（PBKDF2-HMAC-SHA256, 600000 轮）\n"
     "  -s<salt>  指定盐（默认随机 32 字节）\n"
     "  不带 -p 时为无密码仓库：仅混淆，不保密。"},
    {"load",
     "load -p<pwd>\n"
     "  校验密码并解密索引。每次启动进程后都要先 load（无密码仓库除外）。\n"
     "  错误密码会提示 Incorrect password。"},
    {"add",
     "add <src> [内路径] [-r]\n"
     "  <src> 是文件时：加入单个文件。\n"
     "    省略内路径      -> /<文件名>\n"
     "    内路径以 / 结尾 -> 放入该目录，如 add a.txt /docs/\n"
     "    否则            -> 作为完整内部路径，如 add a.txt /docs/a.txt\n"
     "  <src> 是文件夹时：递归导入其下所有文件与子目录（空目录也会保留）。\n"
     "    省略内路径      -> /<文件夹名>/...\n"
     "    指定内路径      -> /backup/proj/...（保留子目录结构）\n"
     "  例：add D:\\proj /backup/proj        # D:\\proj 整棵树 -> /backup/proj/..."},
    {"import",
     "import <目录> [内路径]\n"
     "  把一个本地目录完整镜像进仓库：其下所有文件 + 所有子目录（含空目录）。\n"
     "  与 add <目录> 行为一致；单文件请用 add。\n"
     "  省略 [内路径] -> /<目录名>/...\n"
     "  已存在的路径会跳过并计入 skipped，不会覆盖。\n"
     "  例：import D:\\photos /backup/photos-2026\n"
     "      import D:\\empty-dir /templates/empty"},
    {"get",
     "get <内路径> [目标]\n"
     "  精确命中文件：导出该文件。\n"
     "    [目标] 省略 -> <out 根目录>/<内路径>\n"
     "    [目标] 是目录 -> <目标>/<文件名>\n"
     "    [目标] 是路径 -> 直接写到该路径\n"
     "  精确命中目录：创建该目录（空目录也能还原）。\n"
     "  前缀命中目录：导出整棵子树（保留目录结构，含空目录）。\n"
     "    省略 [目标] -> <out 根目录>/<内路径>\n"
     "  例：get /docs                 -> 导出 /docs 下所有文件\n"
     "      get /docs D:\\restore     -> 导出到 D:\\restore/<相对路径>\n"
     "      get /                    -> 导出整个仓库"},
    {"ls",
     "ls [内路径] [-r]\n"
     "  不带参数：列出仓库根目录的直接子项（目录带 / 后缀）。\n"
     "  带内路径：列出该目录下的直接子项。\n"
     "  -r：递归列出全部条目（含目录）。\n"
     "  例：ls /            ls /docs -r"},
    {"rm",
     "rm <内路径> [-r]\n"
     "  删除单个文件；目录（含其下全部条目）需要 -r。\n"
     "  删除会回收块位图并更新索引。\n"
     "  例：rm /docs/a.txt        rm /docs -r"},
    {"out",
     "out [路径]\n"
     "  不带参数：显示当前默认导出根目录。\n"
     "  带路径：设置为该目录并写入 .block/config.json。\n"
     "  优先级：环境变量 BLOCKSTORAGE_OUT > .block/config.json > C:\\BlockStorage\\extract\n"
     "  例：out D:\\restore       out"},
    {"statu",
     "statu\n"
     "  显示仓库标志（isfind/iscomplete/haskey/hasload）、256 KiB 大块与 4 KiB 小块使用率、\n"
     "  内联文件数、文件/目录数、默认导出根目录。"},
    {"delete",
     "delete\n"
     "  删除当前目录下的整个 .block/ 仓库（不可恢复）。"},
    {"help",
     "help [命令]\n"
     "  不带参数列出所有命令；带命令名显示该命令的详细用法与原理解释。"}
};

Result getStatu(const Args& args) {
    (void)args;
    res_statu.isfind = false;
    res_statu.iscomplete = false;
    res_statu.haskey = false;
    res_statu.hasload = false;
    res_statu.key_sha256.clear();
    res_statu.salt.clear();

    const store::Layout& L = store::layout();
    if (!store::path_exists(L.dir)) {
        res_statu.key.clear();
        return {true, ""};
    }
    res_statu.isfind = true;

    res_statu.iscomplete = store::path_exists(L.index) && store::path_exists(L.key) &&
                           store::path_exists(L.data) && store::path_exists(L.small_data) &&
                           store::path_exists(L.bitmap) && store::path_exists(L.small_bitmap);

    U8list keyfile = read_file(L.key);
    if (keyfile.size() < 64) {
        // 无密码仓库：空 key 文件视为已解锁
        res_statu.haskey = false;
        res_statu.hasload = true;
        res_statu.key.clear();
        return {true, ""};
    }

    res_statu.haskey = true;
    res_statu.salt.assign(keyfile.begin(), keyfile.begin() + 32);
    res_statu.key_sha256.assign(keyfile.begin() + 32, keyfile.begin() + 64);
    if (res_statu.key.size() != 32) {
        res_statu.hasload = false;
        res_statu.key.clear();
    } else {
        res_statu.hasload = true;
    }
    return {true, ""};
}

void cmd_prepare(const Args& args) {
    (void)args;
    res_statu = Status{false, false, false, false, {}, {}, {}};
    std::cout << "BlockStorage v" << VER << std::endl;
    std::cout << "BlockStorage, made by @solitn" << std::endl
              << "Github: https://github.com/solitn/blockstorage" << std::endl;
    getStatu({});
    if (res_statu.isfind) {
        if (res_statu.iscomplete) {
            std::cout << "Repository found" << std::endl;
        } else {
            std::cout << "Repository found, but some files are missing" << std::endl;
        }
    } else {
        std::cout << "Repository not found" << std::endl;
    }
}

Result command(const std::string& input) {
    Args args = SplitCommandLine(input);
    if (args.empty()) {
        return {false, "No command given"};
    }
    auto it = COMMANDS.find(args[0]);
    if (it != COMMANDS.end()) {
        return it->second.fn(args);
    }
    return {false, "Unknown command: " + args[0]};
}

Result cmd_statu(const Args& args) {
    (void)args;
    getStatu({});
    std::cout << "isfind     : " << (res_statu.isfind ? "true" : "false") << "\n";
    std::cout << "iscomplete : " << (res_statu.iscomplete ? "true" : "false") << "\n";
    std::cout << "haskey     : " << (res_statu.haskey ? "true" : "false") << "\n";
    std::cout << "hasload    : " << (res_statu.hasload ? "true" : "false") << "\n";
    if (res_statu.haskey) {
        std::cout << "salt       : " << Base64Encode(res_statu.salt) << "\n";
        std::cout << "key sha256 : " << Base64Encode(res_statu.key_sha256) << "\n";
    }
    if (res_statu.isfind && res_statu.iscomplete) {
        StoreStats s = stats();
        std::cout << "big blocks : " << s.blocks_used << " / " << s.blocks << " used (256 KiB each)\n";
        std::cout << "small blocks: " << s.slots_used << " / " << s.slots << " used (4 KiB each)\n";
        std::cout << "inline     : " << s.inline_files << " file(s)\n";
        if (res_statu.hasload) {
            if (!g_index_loaded) g_index_loaded = load_index(res_statu.key);
            if (g_index_loaded) {
                std::vector<ListRow> rows;
                list_files("/", true, rows);
                std::size_t dirs = 0;
                std::size_t files = 0;
                std::set<std::string> seen;
                for (const ListRow& r : rows) {
                    if (r.dir) {
                        seen.insert(r.path);
                        continue;
                    }
                    ++files;
                    std::string p = r.path;
                    std::size_t pos = 0;
                    while ((pos = p.find('/', pos + 1)) != std::string::npos) {
                        seen.insert(p.substr(0, pos));
                    }
                }
                dirs = seen.size();
                std::cout << "files      : " << files << "\n";
                std::cout << "folders    : " << dirs << "\n";
            }
        }
        std::cout << "out root   : " << OutputRoot() << "\n";
    }
    return {true, ""};
}

Result cmd_delete(const Args& args) {
    (void)args;
    if (!store::path_exists(store::layout().dir)) {
        return {false, "Repository not initialized"};
    }
    std::error_code ec;
    std::filesystem::remove_all(store::layout().dir, ec);
    if (ec) return {false, ec.message()};

    g_index_loaded = false;
    files = json::object();
    res_statu = Status{false, false, false, false, {}, {}, {}};
    return {true, "Repository deleted"};
}

Result cmd_help(const Args& args) {
    if (args.size() >= 2) {
        const std::string& topic = args[1];
        auto it = HELP_DETAILS.find(topic);
        if (it == HELP_DETAILS.end()) {
            return {false, "No detailed help for '" + topic + "', try: help"};
        }
        std::cout << it->second << std::endl;
        return {true, ""};
    }
    std::cout << "Available commands (help <命令> 查看详细用法):" << std::endl;
    for (const auto& [name, entry] : COMMANDS) {
        std::cout << "  " << entry.desc << std::endl;
    }
    std::cout << "\n内部路径：仓库内一律使用 / 分隔，例如 /docs/2024/a.txt\n"
              << "  首次使用：init -p<密码>  ->  add <文件或文件夹>  ->  ls  ->  get <内部路径>\n"
              << "  详细操作指南见仓库根目录 help.md" << std::endl;
    return {true, ""};
}

Result cmd_load(const Args& args) {
    getStatu({});
    if (!res_statu.isfind) return {false, "Cannot load repository: repository not found"};
    if (!res_statu.iscomplete) {
        return {false, "Cannot load repository: repository is incomplete or missing"};
    }
    if (!res_statu.haskey) return {false, "Cannot load repository: no key provided"};
    if (res_statu.hasload) return {false, "Repository already loaded"};
    if (args.size() < 2 || args[1].rfind("-p", 0) != 0) {
        return {false, "Usage: load -p<password>"};
    }

    const std::string password = args[1].substr(2);
    if (password.empty()) return {false, "Password must not be empty"};

    const U8list derived = DeriveKey(U8list(password.begin(), password.end()), res_statu.salt);
    if (derived.empty()) return {false, "Key derivation failed"};
    if (Sha256(derived) != res_statu.key_sha256) {
        return {false, "Incorrect password"};
    }

    res_statu.key = derived;
    res_statu.hasload = true;
    if (!load_index(res_statu.key)) {
        res_statu.key.clear();
        res_statu.hasload = false;
        return {false, "Cannot decrypt repository index"};
    }
    g_index_loaded = true;
    return {true, "Loaded repository"};
}

Result cmd_init(const Args& args) {
    getStatu({});
    if (res_statu.isfind) return {false, "Repository already exists"};

    std::string password;
    U8list salt_bytes;
    bool has_password = false;
    bool has_salt = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.rfind("-p", 0) == 0) {
            has_password = true;
            password = a.substr(2);
        } else if (a.rfind("-s", 0) == 0) {
            has_salt = true;
            salt_bytes = To32(U8list(a.begin() + 2, a.end()));
        } else {
            return {false, "Unknown option: " + a};
        }
    }
    if (has_password && password.empty()) return {false, "Password must not be empty"};

    std::string err;
    if (!store::create_area(err)) return {false, err};

    if (!has_password && !has_salt) {
        g_index_loaded = false;
        return {true, "Repository initialized (no password, data is only obfuscated)"};
    }

    if (!has_salt) {
        std::cout << "No salt provided, generating random salt..." << std::endl;
        salt_bytes = RandomBytes(32);
        if (salt_bytes.size() != 32) return {false, "Random generation failed"};
    }

    const U8list derived =
        DeriveKey(U8list(password.begin(), password.end()), salt_bytes);
    if (derived.empty()) return {false, "Key derivation failed"};
    const U8list digest = Sha256(derived);

    U8list keyfile = salt_bytes;
    keyfile.insert(keyfile.end(), digest.begin(), digest.end());

    std::ofstream out(store::layout().key, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Failed to open key file"};
    out.write(reinterpret_cast<const char*>(keyfile.data()),
              static_cast<std::streamsize>(keyfile.size()));
    out.close();
    if (!out.good()) return {false, "Failed to write key file"};

    res_statu.key = derived;
    res_statu.hasload = true;
    res_statu.haskey = true;
    res_statu.salt = salt_bytes;
    res_statu.key_sha256 = digest;
    g_index_loaded = true;
    files = json::object();
    files["version"] = 4;
    files["files"] = json::object();

    std::cout << "salt (Base64): " << Base64Encode(salt_bytes) << std::endl;
    std::cout << "key  (Base64): " << Base64Encode(derived) << std::endl;
    return {true, "Repository initialized successfully"};
}

Result cmd_add(const Args& args) {
    if (args.size() < 2) return {false, "Usage: add <source> [内路径] [-r]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;

    bool recursive = false;
    std::string src;
    std::string internal;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-r" || args[i] == "--recursive") {
            recursive = true;
            continue;
        }
        if (src.empty()) {
            src = args[i];
        } else if (internal.empty()) {
            internal = args[i];
        } else {
            return {false, "Too many arguments, usage: add <source> [内路径] [-r]"};
        }
    }
    if (src.empty()) return {false, "Usage: add <source> [内路径] [-r]"};

    const fs::path src_path = PathFromUtf8(src);
    std::error_code ec;
    const bool src_is_dir = fs::is_directory(src_path, ec);
    if (src_is_dir) {
        if (internal.empty()) internal = "/" + PathToUtf8(src_path.filename());
        int files = 0;
        int dirs = 0;
        int failed = 0;
        return add_tree(src, internal, res_statu.key, files, dirs, failed);
    }
    if (recursive) {
        return {false, "-r 只能用于文件夹: " + src};
    }
    return add_file(src, internal, res_statu.key);
}

Result cmd_import(const Args& args) {
    if (args.size() < 2) return {false, "Usage: import <目录> [内路径]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;

    std::string src;
    std::string internal;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-r" || args[i] == "--recursive") continue;  // 兼容写法
        if (src.empty()) {
            src = args[i];
        } else if (internal.empty()) {
            internal = args[i];
        } else {
            return {false, "Too many arguments, usage: import <目录> [内路径]"};
        }
    }
    if (src.empty()) return {false, "Usage: import <目录> [内路径]"};

    const fs::path src_path = PathFromUtf8(src);
    std::error_code ec;
    if (!fs::is_directory(src_path, ec)) {
        return {false, "Not a directory: " + src + "（单文件请用 add）"};
    }
    if (internal.empty()) internal = "/" + PathToUtf8(src_path.filename());

    int files = 0;
    int dirs = 0;
    int failed = 0;
    return add_tree(src, internal, res_statu.key, files, dirs, failed);
}

Result cmd_get(const Args& args) {
    if (args.size() < 2) return {false, "Usage: get <内路径> [目标]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    const std::string dst = args.size() >= 3 ? args[2] : std::string();
    return get_file(args[1], dst, res_statu.key);
}

Result cmd_rm(const Args& args) {
    if (args.size() < 2) return {false, "Usage: rm <内路径> [-r]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    bool recursive = false;
    std::string target;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-r" || args[i] == "--recursive") {
            recursive = true;
        } else if (target.empty()) {
            target = args[i];
        } else {
            return {false, "Too many arguments, usage: rm <内路径> [-r]"};
        }
    }
    if (target.empty()) return {false, "Usage: rm <内路径> [-r]"};
    return remove_file(target, res_statu.key, recursive);
}

Result cmd_ls(const Args& args) {
    Result pre = require_loaded();
    if (!pre.status) return pre;

    bool recursive = false;
    std::string prefix;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-r" || args[i] == "--recursive") {
            recursive = true;
        } else if (prefix.empty()) {
            prefix = args[i];
        } else {
            return {false, "Too many arguments, usage: ls [内路径] [-r]"};
        }
    }

    std::vector<ListRow> rows;
    if (!list_files(prefix, recursive, rows)) return {false, "Invalid internal path"};
    if (rows.empty()) return {true, "Repository is empty"};

    std::cout << "TYPE  SIZE          PATH\n";
    for (const ListRow& r : rows) {
        if (r.dir) {
            std::cout << "dir   -             " << r.path << "/\n";
            continue;
        }
        const std::string size = std::to_string(r.size);
        std::cout << "file  " << size;
        if (size.size() < 13) std::cout << std::string(13 - size.size(), ' ');
        std::cout << " " << r.path << "\n";
    }
    return {true, ""};
}

Result cmd_out(const Args& args) {
    if (args.size() >= 2) {
        std::string err;
        if (!SetOutputRoot(args[1], err)) {
            return {false, "Cannot set output root: " + err};
        }
        return {true, "Default extract root set to " + args[1]};
    }
    const std::string from_env = EnvOutputRoot();
    const std::string src = !from_env.empty()
                                ? "environment variable BLOCKSTORAGE_OUT"
                                : (OutputRoot() == DefaultOutputRoot() ? std::string("default")
                                                                      : std::string(".block/config.json"));
    std::cout << "output root : " << OutputRoot() << "\n";
    std::cout << "source      : " << src << "\n";
    return {true, ""};
}

std::vector<std::string> SplitCommandLine(const std::string& cmd) {
    std::vector<std::string> args;
    std::string cur;
    bool inQuotes = false;   // 是否处于引号内
    bool hasContent = false; // 当前参数是否已开始（用于识别空参数 ""）
    size_t i = 0, n = cmd.size();

    while (i < n) {
        char c = cmd[i];

        // 处理连续反斜杠
        if (c == '\\') {
            size_t start = i;
            while (i < n && cmd[i] == '\\') ++i;
            size_t cnt = i - start;

            if (i < n && cmd[i] == '"') {
                cur.append(cnt / 2, '\\');      // 每两个 \ 还原成一个
                if (cnt % 2 == 0) {
                    inQuotes = !inQuotes;       // 偶数个：引号起作用
                    hasContent = true;
                    ++i;
                } else {
                    cur.push_back('"');         // 奇数个：引号是字面量
                    hasContent = true;
                    ++i;
                }
            } else {
                cur.append(cnt, '\\');          // 后面不是引号，原样保留
                hasContent = true;
            }
            continue;
        }

        if (c == '"') {
            if (inQuotes && i + 1 < n && cmd[i + 1] == '"') {
                cur.push_back('"');             // 引号内的 "" -> 一个 "
                i += 2;
            } else {
                inQuotes = !inQuotes;           // 切换引号状态
                ++i;
            }
            hasContent = true;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (hasContent) {
                args.push_back(cur);
                cur.clear();
                hasContent = false;
            }
            ++i;
            continue;
        }

        cur.push_back(c);
        hasContent = true;
        ++i;
    }

    if (hasContent) args.push_back(cur);
    return args;
}
