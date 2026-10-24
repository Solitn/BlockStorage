#include "command.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "encrypt.h"
#include "files.h"
#include "main.h"
#include "store.h"
#include "types.h"

const std::string VER = "0.1";

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
    {"init",   {cmd_init,   "Initialize a new repository, add -p<password> and -s<salt> to encrypt it"}},
    {"load",   {cmd_load,   "Unlock the repository: load -p<password>"}},
    {"add",    {cmd_add,    "Add a file: add <source> [name]"}},
    {"get",    {cmd_get,    "Extract a file: get <name> [destination]"}},
    {"ls",     {cmd_ls,     "List files stored in the repository"}},
    {"rm",     {cmd_rm,     "Remove a file from the repository: rm <name>"}},
    {"statu",  {cmd_statu,  "Show the current status of the repository"}},
    {"delete", {cmd_delete, "Delete the repository"}},
    {"help",   {cmd_help,   "Show this help message"}}
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
                           store::path_exists(L.data) && store::path_exists(L.small) &&
                           store::path_exists(L.bitmap) && store::path_exists(L.smbitmap);

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
        std::cout << "blocks     : " << s.blocks_used << " / " << s.blocks
                  << " used (" << store::kBlockRec << " B each)\n";
        std::cout << "small slots: " << s.slots_used << " / " << s.slots
                  << " used (" << store::kSmallRec << " B each)\n";
        if (res_statu.hasload) {
            if (!g_index_loaded) g_index_loaded = load_index(res_statu.key);
            if (g_index_loaded) {
                std::vector<std::pair<std::string, std::uint64_t>> list;
                list_files(list);
                std::cout << "files      : " << list.size() << "\n";
            }
        }
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
    (void)args;
    std::cout << "Available commands:" << std::endl;
    for (const auto& [name, entry] : COMMANDS) {
        std::cout << "  " << name << ": " << entry.desc << std::endl;
    }
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
    files["version"] = 1;
    files["files"] = json::object();

    std::cout << "salt (Base64): " << Base64Encode(salt_bytes) << std::endl;
    std::cout << "key  (Base64): " << Base64Encode(derived) << std::endl;
    return {true, "Repository initialized successfully"};
}

Result cmd_add(const Args& args) {
    if (args.size() < 2) return {false, "Usage: add <source> [name]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    const std::string name = args.size() >= 3 ? args[2] : std::string();
    return add_file(args[1], name, res_statu.key);
}

Result cmd_get(const Args& args) {
    if (args.size() < 2) return {false, "Usage: get <name> [destination]"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    const std::string dst = args.size() >= 3 ? args[2] : std::string();
    return get_file(args[1], dst, res_statu.key);
}

Result cmd_rm(const Args& args) {
    if (args.size() < 2) return {false, "Usage: rm <name>"};
    Result pre = require_loaded();
    if (!pre.status) return pre;
    return remove_file(args[1], res_statu.key);
}

Result cmd_ls(const Args& args) {
    (void)args;
    Result pre = require_loaded();
    if (!pre.status) return pre;
    std::vector<std::pair<std::string, std::uint64_t>> list;
    list_files(list);
    if (list.empty()) return {true, "Repository is empty"};
    std::cout << "NAME                             SIZE      KIND\n";
    for (const auto& [name, size] : list) {
        std::cout << name;
        if (name.size() < 33) std::cout << std::string(33 - name.size(), ' ');
        else std::cout << " ";
        std::cout << size;
        if (std::to_string(size).size() < 10) {
            std::cout << std::string(10 - std::to_string(size).size(), ' ');
        }
        std::cout << (size <= store::kSmallSize ? "small" : "blk") << "\n";
    }
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
