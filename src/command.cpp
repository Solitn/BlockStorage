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
    {"init",   {cmd_init,   "init [-p<pwd>] [-s<salt>]      create a repository (optionally encrypted)"}},
    {"load",   {cmd_load,   "load -p<pwd>                   unlock the repository"}},
    {"add",    {cmd_add,    "add <src> [path] [-r]          add a file or a folder (keeps subfolders)"}},
    {"import", {cmd_import, "import <dir> [path]            mirror a folder: files + dirs (incl. empty)"}},
    {"get",    {cmd_get,    "get <path> [target]            extract a file, a folder or the whole repo"}},
    {"ls",     {cmd_ls,     "ls [path] [-r]                 list entries (tree with -r)"}},
    {"rm",     {cmd_rm,     "rm <path> [-r]                 remove a file, or a folder with -r"}},
    {"out",    {cmd_out,    "out [path]                     show / set the default extract root"}},
    {"statu",  {cmd_statu,  "statu                          show repository status"}},
    {"delete", {cmd_delete, "delete                         delete the whole repository"}},
    {"help",   {cmd_help,   "help [command]                 show help (per-command details)"}}
};

// Per-command detailed help
const std::map<std::string, std::string> HELP_DETAILS = {
    {"init",
     "init [-p<pwd>] [-s<salt>]\n"
     "  Create a .block/ repository in the current directory.\n"
     "  -p<pwd>   encrypt with a password (PBKDF2-HMAC-SHA256, 600000 rounds)\n"
     "  -s<salt>  provide a salt (default: random 32 bytes)\n"
     "  Without -p the repository is unencrypted: obfuscation only, no confidentiality."},
    {"load",
     "load -p<pwd>\n"
     "  Verify the password and decrypt the index. Required after every start\n"
     "  (except for keyless repositories). A wrong password reports \"Incorrect password\"."},
    {"add",
     "add <src> [path] [-r]\n"
     "  <src> is a file:\n"
     "    path omitted      -> /<filename>\n"
     "    path ends with /  -> put into that folder, e.g. add a.txt /docs/\n"
     "    otherwise         -> use as the full internal path, e.g. add a.txt /docs/a.txt\n"
     "  <src> is a folder (or -r is given): import all files and subfolders\n"
     "  recursively, empty folders included.\n"
     "    path omitted      -> /<foldername>/...\n"
     "    path given        -> /backup/proj/... (subfolder structure is kept)\n"
     "  Example: add D:\\proj /backup/proj"},
    {"import",
     "import <dir> [path]\n"
     "  Mirror a local directory into the repository: every file and every\n"
     "  subfolder, including empty ones.\n"
     "  Same behaviour as 'add <dir>'; use 'add' for a single file.\n"
     "  path omitted -> /<dirname>/...\n"
     "  Any duplicate or conflicting path aborts the whole import (nothing is written).\n"
     "  Example: import D:\\photos /backup/photos-2026"},
    {"get",
     "get <path> [target]\n"
     "  Exact file: extract that file.\n"
     "    target omitted   -> <out root>/<internal path>\n"
     "    target is a dir  -> <target>/<filename>\n"
     "    target is a path -> write to that path\n"
     "  Folder (exact or prefix match): export the whole subtree, including\n"
     "  empty folders and keeping the structure.\n"
     "    target omitted   -> <out root>/<internal path>\n"
     "  Examples: get /docs                export every file under /docs\n"
     "            get /docs D:\\restore     export to D:\\restore/<relative path>\n"
     "            get /                    export the whole repository"},
    {"ls",
     "ls [path] [-r]\n"
     "  No arguments: list direct children of the repository root (folders end with /).\n"
     "  With a path: list direct children of that folder.\n"
     "  -r: list every entry recursively (folders included).\n"
     "  Examples: ls /            ls /docs -r"},
    {"rm",
     "rm <path> [-r]\n"
     "  Remove a single file; folders (and everything under them) require -r.\n"
     "  Blocks are returned to the bitmap and the index is rewritten.\n"
     "  Examples: rm /docs/a.txt        rm /docs -r"},
    {"out",
     "out [path]\n"
     "  No arguments: show the current default extract root and where it comes from.\n"
     "  With a path: set it and store it in .block/config.json.\n"
     "  Priority: BLOCKSTORAGE_OUT > .block/config.json > <cwd>\\extract\n"
     "  Examples: out D:\\restore       out"},
    {"statu",
     "statu\n"
     "  Show repository flags (isfind/iscomplete/haskey/hasload), usage of 256 KiB\n"
     "  big blocks and 4 KiB small blocks, inline file count, file/folder counts and\n"
     "  the default extract root."},
    {"delete",
     "delete\n"
     "  Delete the whole .block/ repository in the current directory (irreversible)."},
    {"help",
     "help [command]\n"
     "  Without arguments: list every command.\n"
     "  With a command name: show detailed usage for that command."}
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
    std::cout << "Available commands (use 'help <command>' for details):" << std::endl;
    for (const auto& [name, entry] : COMMANDS) {
        std::cout << "  " << entry.desc << std::endl;
    }
    std::cout << "\nInternal paths use '/' as separator, e.g. /docs/2024/a.txt\n"
              << "  Quick start: init -p<pwd>  ->  add <file-or-folder>  ->  ls  ->  get <path>\n"
              << "  Full guide: see help.md in the repository root" << std::endl;
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
    if (args.size() < 2) return {false, "Usage: add <source> [path] [-r]"};
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
            return {false, "Too many arguments, usage: add <source> [path] [-r]"};
        }
    }
    if (src.empty()) return {false, "Usage: add <source> [path] [-r]"};

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
        return {false, "-r can only be used with a folder: " + src};
    }
    return add_file(src, internal, res_statu.key);
}

Result cmd_import(const Args& args) {
    if (args.size() < 2) return {false, "Usage: import <dir> [path]"};
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
            return {false, "Too many arguments, usage: import <dir> [path]"};
        }
    }
    if (src.empty()) return {false, "Usage: import <dir> [path]"};

    const fs::path src_path = PathFromUtf8(src);
    std::error_code ec;
    if (!fs::is_directory(src_path, ec)) {
        return {false, "Not a directory: " + src + " (use 'add' for a single file)"};
    }
    if (internal.empty()) internal = "/" + PathToUtf8(src_path.filename());

    int files = 0;
    int dirs = 0;
    int failed = 0;
    return add_tree(src, internal, res_statu.key, files, dirs, failed);
}

Result cmd_get(const Args& args) {
    if (args.size() < 2) return {false, "Usage: get <path> [target]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    const std::string dst = args.size() >= 3 ? args[2] : std::string();
    return get_file(args[1], dst, res_statu.key);
}

Result cmd_rm(const Args& args) {
    if (args.size() < 2) return {false, "Usage: rm <path> [-r]"};
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
            return {false, "Too many arguments, usage: rm <path> [-r]"};
        }
    }
    if (target.empty()) return {false, "Usage: rm <path> [-r]"};
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
            return {false, "Too many arguments, usage: ls [path] [-r]"};
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
