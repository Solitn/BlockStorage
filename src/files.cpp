#include "files.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "encrypt.h"
#include "main.h"
#include "store.h"
#include "types.h"
#include "utf8.h"
#include "version.h"

namespace fs = std::filesystem;

namespace {

// 无密码仓库使用的固定密钥（仅混淆，不提供机密性）
const U8list kDefaultKey = {
    0x5E, 0x1B, 0x9C, 0x47, 0xD2, 0x80, 0x3A, 0xF6,
    0x28, 0xB5, 0x6D, 0xE1, 0x03, 0x97, 0x4C, 0xAA,
    0x71, 0x0F, 0xC8, 0x34, 0x96, 0x5B, 0xE2, 0x19,
    0x8E, 0xD4, 0x27, 0x63, 0xFA, 0x08, 0xB1, 0x4D
};

// 索引里记录的导出根（避免每次读盘）
std::string g_output_root;

// bit 表（空闲链 + 文件链）
store::BitTable g_bit;
bool g_bit_loaded = false;

json MakeDirNode() {
    json n;
    n["kind"] = "dir";
    n["children"] = json::object();
    return n;
}

json empty_index() {
    json j;
    j["version"] = BS_INDEX_VERSION;
    j["config"] = json::object();
    j["tree"] = MakeDirNode();
    return j;
}

// 目录树根；结构缺失时重建空树
json& index_tree() {
    if (!files.is_object() || !files.contains("tree") || !files["tree"].is_object() ||
        files["tree"].value("kind", "") != "dir") {
        files = empty_index();
    }
    return files["tree"];
}

bool ensure_bit() {
    if (g_bit_loaded) return true;
    std::string err;
    if (!g_bit.load(store::layout().bit, err)) return false;
    g_bit_loaded = true;
    return true;
}

// 内部路径的大小写无关比较键（Windows 路径大小写不敏感）
std::string lower_path(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

// 在 dir.children 中按大小写不敏感查找；actual 返回仓库里的真实写法
json* child_of(json& dir, const std::string& name, std::string* actual) {
    if (!dir.contains("children") || !dir["children"].is_object()) return nullptr;
    json& ch = dir["children"];
    const std::string lname = lower_path(name);
    for (auto it = ch.begin(); it != ch.end(); ++it) {
        if (lower_path(it.key()) != lname) continue;
        if (actual) *actual = it.key();
        return &it.value();
    }
    return nullptr;
}

const json* child_of(const json& dir, const std::string& name, std::string* actual) {
    if (!dir.contains("children") || !dir["children"].is_object()) return nullptr;
    const json& ch = dir["children"];
    const std::string lname = lower_path(name);
    for (auto it = ch.begin(); it != ch.end(); ++it) {
        if (lower_path(it.key()) != lname) continue;
        if (actual) *actual = it.key();
        return &it.value();
    }
    return nullptr;
}

std::string child_path(const std::string& parent, const std::string& name) {
    if (parent.empty() || parent == "/") return "/" + name;
    return parent + "/" + name;
}

std::uint64_t entry_size(const json& node) {
    return node.value("size", std::uint64_t{0});
}

bool is_dir_node(const json& node) { return node.value("kind", "") == "dir"; }

// 逐级创建目录；返回最终节点，失败返回 nullptr（某级是文件）
json* ensure_dir_path(json& root, const std::vector<std::string>& segs, std::string& canon) {
    json* cur = &root;
    canon.clear();
    for (const std::string& s : segs) {
        std::string actual;
        json* nxt = child_of(*cur, s, &actual);
        if (!nxt) {
            (*cur)["children"][s] = MakeDirNode();
            nxt = &(*cur)["children"][s];
            actual = s;
        } else if (!is_dir_node(*nxt)) {
            return nullptr;
        }
        canon = child_path(canon, actual);
        cur = nxt;
    }
    return cur;
}

struct PathCheck {
    std::string canonical;  // 若可写入，返回仓库里的规范写法
    std::string conflict;   // 非空表示冲突的既有路径
};

// 只读检查：目标路径是否与现有条目冲突（大小写不敏感）
// want_dir = true 时已存在的同名目录视为可复用
PathCheck check_path(json& root, const std::string& name, bool want_dir) {
    PathCheck out;
    const std::vector<std::string> segs = SplitInternalPath(name);
    if (segs.empty()) {
        out.conflict = "/";
        return out;
    }
    json* cur = &root;
    std::string canon;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        std::string actual;
        json* nxt = child_of(*cur, segs[i], &actual);
        if (!nxt) {
            canon = child_path(canon, segs[i]);
            for (std::size_t k = i + 1; k < segs.size(); ++k) canon = child_path(canon, segs[k]);
            out.canonical = canon;
            return out;
        }
        canon = child_path(canon, actual);
        if (!is_dir_node(*nxt)) {
            out.conflict = canon;
            return out;
        }
        cur = nxt;
    }
    std::string leaf_actual;
    if (json* leaf = child_of(*cur, segs.back(), &leaf_actual)) {
        const std::string p = child_path(canon, leaf_actual);
        if (want_dir && is_dir_node(*leaf)) {
            out.canonical = p;
            return out;
        }
        out.conflict = p;
        return out;
    }
    out.canonical = child_path(canon, segs.back());
    return out;
}

bool write_all(const fs::path& path, const U8list& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return out.good();
}

// 内部路径 -> 本地文件系统路径（root 为 UTF-8，段按 UTF-8 转宽字符）
fs::path to_fs_path(const std::string& root, const std::string& internal) {
    fs::path p = PathFromUtf8(root);
    for (const std::string& seg : SplitInternalPath(internal)) p /= Utf8ToWide(seg);
    return p;
}

bool parent_dirs_ok(const fs::path& p, std::string& err) {
    if (!p.has_parent_path()) return true;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec && !fs::is_directory(p.parent_path())) {
        err = ec.message();
        return false;
    }
    return true;
}

// 释放一个条目占用的所有块（写回 bit 内存表，调用方负责 save）
bool free_blocks_of(const json& entry) {
    const std::string kind = entry.value("kind", "");
    if (kind != "small" && kind != "blk") return true;
    const int tier = (kind == "small") ? store::kTierSmall : store::kTierBig;
    std::int64_t id = entry.value("head", std::int64_t{-1});
    std::uint64_t guard = 0;
    while (id != -1) {
        const std::int64_t nxt = g_bit.next_of(tier, id);
        if (!g_bit.release(tier, id)) return false;
        id = nxt;
        if (++guard > g_bit.cap[tier]) return false;  // 链成环
    }
    return true;
}

// 读取一条文件链并解密
Result read_chain(const json& entry, const U8list& master, U8list& plain) {
    const std::string kind = entry.value("kind", "");
    const bool small = (kind == "small");
    const std::size_t payload_size = small ? store::kSmallBlock : store::kLargeBlock;
    const std::size_t rec_size = small ? store::kSmallRec : store::kLargeRec;
    const std::uint8_t domain = small ? 2 : 1;
    const std::string& data_path = small ? store::layout().small : store::layout().data;

    if (!entry.contains("head") || !entry.contains("size")) {
        return {false, "Corrupted index entry"};
    }
    const std::uint64_t size = entry.value("size", std::uint64_t{0});
    const std::uint64_t want = (size + payload_size - 1) / payload_size;

    std::int64_t id = entry.value("head", std::int64_t{-1});
    std::uint64_t got = 0;
    while (id != -1) {
        if (++got > want) return {false, "Block chain is longer than expected"};
        U8list rec;
        if (!store::record_read(data_path, rec_size, static_cast<std::uint64_t>(id), rec)) {
            return {false, "Cannot read block " + std::to_string(id)};
        }
        U8list payload;
        if (!GcmOpen(rec, SubKey(master, domain, static_cast<std::uint64_t>(id)), payload)) {
            return {false, "Data authentication failed (wrong key or corrupted block)"};
        }
        plain.insert(plain.end(), payload.begin(), payload.end());
        id = g_bit.next_of(small ? store::kTierSmall : store::kTierBig, id);
    }
    if (got != want) return {false, "Truncated data (missing blocks)"};
    if (plain.size() < size) return {false, "Truncated data"};
    plain.resize(static_cast<std::size_t>(size));
    return {true, ""};
}

}  // namespace

// ---------------------------------------------------------------- 基础

U8list RepoKey(const U8list& key) {
    return key.size() == 32 ? key : kDefaultKey;
}

bool check_dir() {
    return store::path_exists(store::layout().dir);
}

std::string NormalizeInternalPath(const std::string& raw) {
    std::string s;
    s.reserve(raw.size() + 1);
    for (char c : raw) s.push_back(c == '\\' ? '/' : c);

    std::vector<std::string> segs;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            if (!cur.empty()) {
                segs.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) segs.push_back(cur);

    for (const std::string& seg : segs) {
        if (seg == "." || seg == "..") return {};
        if (seg.find(':') != std::string::npos) return {};  // 拒绝盘符
    }
    if (segs.empty()) return "/";
    std::string out;
    for (const std::string& seg : segs) out += "/" + seg;
    return out;
}

std::vector<std::string> SplitInternalPath(const std::string& path) {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) {
                segs.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
}

std::string JoinInternalPath(const std::string& base, const std::string& rel) {
    if (rel.empty()) return NormalizeInternalPath(base);
    if (rel.front() == '/' || rel.front() == '\\') return NormalizeInternalPath(rel);
    std::string b = base.empty() ? std::string("/") : base;
    if (b.back() != '/') b += '/';
    return NormalizeInternalPath(b + rel);
}

std::string IndexOutputRoot() {
    return g_output_root;
}

bool SetIndexOutputRoot(const std::string& path) {
    g_output_root = path;
    if (!files.is_object()) files = empty_index();
    if (!files.contains("config") || !files["config"].is_object()) files["config"] = json::object();
    if (path.empty()) {
        files["config"].erase("output_root");
    } else {
        files["config"]["output_root"] = path;
    }
    return true;
}

bool load_index(const U8list& key) {
    if (!check_dir()) return false;
    U8list raw = read_file(store::layout().index);
    if (raw.empty()) {
        files = empty_index();
        g_output_root.clear();
        g_bit_loaded = false;
        return true;
    }
    json j = decrypt_json(raw, RepoKey(key));
    if (j.is_null() || !j.is_object()) return false;
    if (j.value("version", 0) != BS_INDEX_VERSION) return false;  // 只支持一种索引版本
    if (!j.contains("tree") || !j["tree"].is_object() || !is_dir_node(j["tree"])) return false;

    files = j;
    g_output_root = files.value("config", json::object()).value("output_root", "");
    g_bit_loaded = false;
    return true;
}

bool save_index(const U8list& key) {
    if (!check_dir()) return false;
    if (!files.is_object()) files = empty_index();
    files["version"] = BS_INDEX_VERSION;
    if (!files.contains("config") || !files["config"].is_object()) files["config"] = json::object();
    if (g_output_root.empty()) {
        files["config"].erase("output_root");
    } else {
        files["config"]["output_root"] = g_output_root;
    }
    U8list raw = encrypt_json(files, RepoKey(key));
    if (raw.empty()) return false;
    return write_all(store::layout().index, raw);
}

// ---------------------------------------------------------------- 写入

Result add_file(const std::string& src, const std::string& internal_in, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};

    const fs::path src_path = PathFromUtf8(src);
    std::error_code ec;
    if (!fs::exists(src_path, ec) || fs::is_directory(src_path, ec)) {
        return {false, "Source file not found: " + src};
    }
    std::uint64_t src_size = 0;
    if (!store::file_size_of(src_path, src_size)) return {false, "Cannot stat source file"};

    U8list plain = read_file(src_path);
    if (plain.size() != src_size) return {false, "Cannot read source file"};

    const std::string base_name = PathToUtf8(src_path.filename());

    // 以 '/' 结尾表示"放进这个目录"
    const bool as_dir = !internal_in.empty() &&
                        (internal_in.back() == '/' || internal_in.back() == '\\');
    std::string name;
    if (internal_in.empty()) {
        name = NormalizeInternalPath(base_name);
    } else if (as_dir) {
        std::string base = NormalizeInternalPath(internal_in);
        if (base.empty()) return {false, "Invalid internal path"};
        name = (base == "/" ? std::string() : base) + "/" + base_name;
    } else {
        name = NormalizeInternalPath(internal_in);
    }
    if (name.empty()) return {false, "Invalid internal path"};
    if (name == "/") return {false, "Internal path must not be the repository root"};

    const std::vector<std::string> segs = SplitInternalPath(name);
    json& root = index_tree();

    // 先做只读冲突检查，避免失败时留下半截目录
    const PathCheck chk = check_path(root, name, false);
    if (!chk.conflict.empty()) {
        return {false, "Duplicate path: " + name + " conflicts with existing " + chk.conflict};
    }

    // 逐级走到父目录；缺少的目录自动建立
    const std::vector<std::string> parent_segs(segs.begin(), segs.end() - 1);
    std::string canon;
    json* parent = ensure_dir_path(root, parent_segs, canon);
    if (!parent) return {false, "Invalid internal path: " + name};

    const std::string leaf = segs.back();
    const std::string full = child_path(canon, leaf);
    const U8list master = RepoKey(key);
    const std::string digest = Base64Encode(Sha256(plain));

    // 1) 极小文件：内容直接 base64 写进索引（索引本身是 AES-GCM 加密的）
    if (plain.size() <= store::kTinySize) {
        json entry;
        entry["kind"] = "inline";
        entry["size"] = plain.size();
        entry["sha256"] = digest;
        entry["data"] = Base64Encode(plain);
        (*parent)["children"][leaf] = entry;
        if (!save_index(master)) {
            (*parent)["children"].erase(leaf);
            return {false, "Cannot write index"};
        }
        return {true, "Added " + full + " (" + std::to_string(plain.size()) + " bytes, inline)"};
    }

    // 2) ≤256 KiB 走 4 KiB 小块区，更大走 256 KiB 大块区
    const bool small = plain.size() <= store::kLargeBlock;
    const int tier = small ? store::kTierSmall : store::kTierBig;
    const std::size_t payload_size = small ? store::kSmallBlock : store::kLargeBlock;
    const std::size_t rec_size = small ? store::kSmallRec : store::kLargeRec;
    const std::uint8_t domain = small ? 2 : 1;
    const std::string& data_path = small ? store::layout().small : store::layout().data;

    if (!ensure_bit()) return {false, "Cannot read bit file"};

    const std::uint64_t nblocks = (plain.size() + payload_size - 1) / payload_size;
    std::vector<std::int64_t> ids;
    ids.reserve(static_cast<std::size_t>(nblocks));

    auto rollback = [&]() {
        for (std::int64_t id : ids) g_bit.release(tier, id);
        std::string ignored;
        g_bit.save(store::layout().bit, ignored);
    };

    for (std::uint64_t i = 0; i < nblocks; ++i) {
        const std::int64_t id = g_bit.alloc(tier);
        if (id < 0) {
            rollback();
            return {false, "Cannot allocate a data block"};
        }
        ids.push_back(id);

        U8list payload(payload_size, 0);
        const std::uint64_t offset = i * payload_size;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(payload_size, plain.size() - offset));
        std::copy(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                  plain.begin() + static_cast<std::ptrdiff_t>(offset + take),
                  payload.begin());

        const U8list rec = GcmSeal(payload, SubKey(master, domain, static_cast<std::uint64_t>(id)));
        if (rec.size() != rec_size ||
            !store::record_write(data_path, rec_size, static_cast<std::uint64_t>(id), rec)) {
            rollback();
            return {false, "Cannot write data block"};
        }
    }

    // 文件链：块之间的 next 全部记在 bit 里，索引只存 head/tail
    for (std::size_t i = 0; i < ids.size(); ++i) {
        g_bit.set_next(tier, ids[i], (i + 1 < ids.size()) ? ids[i + 1] : -1);
    }
    std::string bit_err;
    if (!g_bit.save(store::layout().bit, bit_err)) {
        rollback();
        return {false, "Cannot update bit file: " + bit_err};
    }

    json entry;
    entry["kind"] = small ? "small" : "blk";
    entry["size"] = plain.size();
    entry["sha256"] = digest;
    entry["head"] = ids.front();
    entry["tail"] = ids.back();
    (*parent)["children"][leaf] = entry;

    if (!save_index(master)) {
        (*parent)["children"].erase(leaf);
        rollback();
        return {false, "Cannot write index"};
    }

    return {true, "Added " + full + " (" + std::to_string(plain.size()) + " bytes, " +
                      std::to_string(nblocks) + (small ? " x4k)" : " x256k)")};
}

Result add_tree(const std::string& src_dir, const std::string& internal_prefix,
                const U8list& key, int& files_added, int& dirs_added, int& failed) {
    files_added = 0;
    dirs_added = 0;
    failed = 0;
    if (!check_dir()) return {false, "Repository not initialized"};

    const fs::path root_path = PathFromUtf8(src_dir);
    std::error_code ec;
    if (!fs::is_directory(root_path, ec)) {
        return {false, "Not a directory: " + src_dir};
    }

    std::string prefix = internal_prefix.empty() ? "/" : NormalizeInternalPath(internal_prefix);
    if (prefix.empty()) return {false, "Invalid internal path"};

    json& root = index_tree();

    std::vector<fs::path> files_found;
    std::vector<fs::path> dirs_found;
    for (fs::recursive_directory_iterator it(root_path, fs::directory_options::skip_permission_denied,
                                             ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            dirs_found.push_back(it->path());
        } else {
            files_found.push_back(it->path());
        }
    }
    std::sort(files_found.begin(), files_found.end());
    std::sort(dirs_found.begin(), dirs_found.end());

    auto relative_of = [&root_path](const fs::path& p, std::string& out) -> bool {
        std::error_code rel_ec;
        fs::path rel = fs::relative(p, root_path, rel_ec);
        if (rel_ec) return false;
        out.clear();
        for (const auto& part : rel) {
            if (!out.empty()) out += '/';
            out += PathToUtf8(part);
        }
        return true;
    };

    const std::string base = (prefix == "/") ? std::string() : prefix;

    // ---- 预扫描：任何重名/父子冲突都整批中止（不做半截导入）----
    std::vector<std::pair<fs::path, std::string>> file_plan;
    std::vector<std::string> dir_plan;
    std::vector<std::string> conflicts;

    if (prefix != "/") {
        const PathCheck chk = check_path(root, prefix, true);
        if (!chk.conflict.empty()) {
            conflicts.push_back(prefix + " (conflicts with " + chk.conflict + ")");
        } else {
            dir_plan.push_back(chk.canonical);
        }
    }

    for (const fs::path& d : dirs_found) {
        std::string rel;
        if (!relative_of(d, rel)) {
            ++failed;
            continue;
        }
        const std::string target = (base.empty() ? std::string() : base) + "/" + rel;
        const PathCheck chk = check_path(root, target, true);
        if (!chk.conflict.empty()) {
            conflicts.push_back(target + " (conflicts with " + chk.conflict + ")");
        } else {
            dir_plan.push_back(chk.canonical);
        }
    }

    for (const fs::path& p : files_found) {
        std::string rel;
        if (!relative_of(p, rel)) {
            ++failed;
            continue;
        }
        const std::string target = (base.empty() ? std::string() : base) + "/" + rel;
        const PathCheck chk = check_path(root, target, false);
        if (!chk.conflict.empty()) {
            conflicts.push_back(target + " (conflicts with " + chk.conflict + ")");
        } else {
            file_plan.emplace_back(p, chk.canonical);
        }
    }

    if (!conflicts.empty()) {
        std::string detail;
        const std::size_t show = std::min<std::size_t>(conflicts.size(), 3);
        for (std::size_t i = 0; i < show; ++i) {
            if (i) detail += "; ";
            detail += conflicts[i];
        }
        if (conflicts.size() > show) {
            detail += "; ... and " + std::to_string(conflicts.size() - show) + " more";
        }
        return {false, "Import aborted, " + std::to_string(conflicts.size()) +
                           " duplicate/conflicting path(s): " + detail};
    }

    // ---- 执行：先目录（含空目录），后文件 ----
    for (const std::string& d : dir_plan) {
        std::string canon;
        if (ensure_dir_path(root, SplitInternalPath(d), canon)) ++dirs_added;
    }

    std::string first_error;
    for (const auto& item : file_plan) {
        Result r = add_file(PathToUtf8(item.first), item.second, key);
        if (r.status) {
            ++files_added;
        } else {
            ++failed;
            if (first_error.empty()) first_error = r.message;
        }
    }

    if (files_added == 0 && dirs_added == 0 && failed > 0) {
        return {false, "Import failed: " + first_error};
    }

    if (!save_index(RepoKey(key))) return {false, "Cannot write index"};

    std::string msg = "Imported " + std::to_string(files_added) + " file(s), " +
                      std::to_string(dirs_added) + " dir(s) into " +
                      (prefix == "/" ? std::string("/") : prefix);
    if (failed) msg += ", " + std::to_string(failed) + " skipped (" + first_error + ")";
    return {true, msg};
}

// ---------------------------------------------------------------- 读取

namespace {

Result export_one(const std::string& internal, const json& entry, const fs::path& out_path,
                  const U8list& master) {
    U8list plain;
    const std::string kind = entry.value("kind", "");

    if (kind == "inline") {
        if (!entry.contains("data") || !Base64Decode(entry["data"].get<std::string>(), plain)) {
            return {false, "Corrupted inline data for " + internal};
        }
    } else if (kind == "small" || kind == "blk") {
        Result r = read_chain(entry, master, plain);
        if (!r.status) return {false, r.message + ": " + internal};
    } else {
        return {false, "Unknown entry kind for " + internal};
    }

    const std::uint64_t size = entry_size(entry);
    if (plain.size() < size) return {false, "Truncated data for " + internal};
    plain.resize(static_cast<std::size_t>(size));

    if (entry.contains("sha256")) {
        const std::string want = entry["sha256"].get<std::string>();
        if (Base64Encode(Sha256(plain)) != want) {
            return {false, "Checksum mismatch for " + internal};
        }
    }

    std::string err;
    if (!parent_dirs_ok(out_path, err)) return {false, "Cannot create directory: " + err};
    if (!write_all(out_path, plain)) return {false, "Cannot write " + PathToUtf8(out_path)};
    return {true, ""};
}

// 导出目录内容：目标路径 = base + 完整内部路径（保留原有层级）
void export_children(const std::string& ipath, const json& dir, const fs::path& base,
                     const U8list& master, int& files, int& dirs, std::string& first_error) {
    if (!dir.contains("children") || !dir["children"].is_object()) return;
    for (auto it = dir["children"].begin(); it != dir["children"].end(); ++it) {
        const std::string cpath = child_path(ipath, it.key());
        fs::path out = base;
        for (const std::string& seg : SplitInternalPath(cpath)) out /= Utf8ToWide(seg);

        const json& node = it.value();
        if (is_dir_node(node)) {
            std::error_code ec;
            fs::create_directories(out, ec);
            if (ec && !fs::is_directory(out)) {
                if (first_error.empty()) first_error = "Cannot create directory " + ec.message();
                continue;
            }
            ++dirs;
            export_children(cpath, node, base, master, files, dirs, first_error);
        } else {
            Result r = export_one(cpath, node, out, master);
            if (r.status) {
                ++files;
            } else if (first_error.empty()) {
                first_error = r.message;
            }
        }
    }
}

}  // namespace

Result get_file(const std::string& internal_in, const std::string& dst_in, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};

    const std::string internal = NormalizeInternalPath(internal_in);
    if (internal.empty()) return {false, "Invalid internal path"};
    if (!ensure_bit()) return {false, "Cannot read bit file"};

    json& root = index_tree();
    json* node = &root;
    std::string canon;
    for (const std::string& seg : SplitInternalPath(internal)) {
        std::string actual;
        json* nxt = child_of(*node, seg, &actual);
        if (!nxt) return {false, "No such file or directory in repository: " + internal};
        node = nxt;
        canon = child_path(canon, actual);
    }
    const std::string ipath = canon.empty() ? "/" : canon;
    const U8list master = RepoKey(key);

    // 1) 单个文件
    if (!is_dir_node(*node)) {
        fs::path out_path;
        if (dst_in.empty()) {
            out_path = to_fs_path(OutputRoot(), ipath);
        } else {
            const fs::path dst = PathFromUtf8(dst_in);
            std::error_code ec;
            if (fs::is_directory(dst, ec)) {
                // 目标是目录：保留仓库内的完整层级
                out_path = dst;
                for (const std::string& seg : SplitInternalPath(ipath)) out_path /= Utf8ToWide(seg);
            } else {
                out_path = dst;
            }
        }
        Result r = export_one(ipath, *node, out_path, master);
        if (!r.status) return r;
        return {true, "Extracted " + ipath + " -> " + PathToUtf8(out_path) + " (" +
                          std::to_string(entry_size(*node)) + " bytes)"};
    }

    // 2) 目录：导出整棵子树，目标 = base + 完整内部路径
    const fs::path base = dst_in.empty() ? PathFromUtf8(OutputRoot()) : PathFromUtf8(dst_in);
    std::string err;
    if (!EnsureDirectory(PathToUtf8(base), err)) {
        return {false, "Cannot create " + PathToUtf8(base) + ": " + err};
    }

    // 顶层目录本身也建出来（不计入 dirs）
    if (ipath != "/") {
        fs::path self = base;
        for (const std::string& seg : SplitInternalPath(ipath)) self /= Utf8ToWide(seg);
        std::error_code ec;
        fs::create_directories(self, ec);
    }

    int files = 0;
    int dirs = 0;
    std::string first_error;
    export_children(ipath, *node, base, master, files, dirs, first_error);

    if (files == 0 && dirs == 0) {
        if (!first_error.empty()) return {false, "Extract failed: " + first_error};
        return {true, "Created directory " + PathToUtf8(base)};
    }

    return {true, "Extracted " + std::to_string(files) + " file(s), " + std::to_string(dirs) +
                      " dir(s) from " + ipath + " -> " + PathToUtf8(base)};
}

// ---------------------------------------------------------------- 删除

namespace {

std::uint64_t count_nodes(const json& node) {
    if (!is_dir_node(node)) return 1;
    std::uint64_t n = 1;
    if (node.contains("children") && node["children"].is_object()) {
        for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
            n += count_nodes(it.value());
        }
    }
    return n;
}

bool free_subtree(const json& node, std::uint64_t& guard) {
    if (!is_dir_node(node)) return free_blocks_of(node);
    if (!node.contains("children") || !node["children"].is_object()) return true;
    for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
        if (++guard > 100000000ULL) return false;
        if (!free_subtree(it.value(), guard)) return false;
    }
    return true;
}

}  // namespace

Result remove_file(const std::string& internal_in, const U8list& key, bool recursive) {
    if (!check_dir()) return {false, "Repository not initialized"};

    const std::string internal = NormalizeInternalPath(internal_in);
    if (internal.empty()) return {false, "Invalid internal path"};
    if (internal == "/") return {false, "Refusing to remove the repository root"};
    if (!ensure_bit()) return {false, "Cannot read bit file"};

    json& root = index_tree();
    const std::vector<std::string> segs = SplitInternalPath(internal);

    json* parent = &root;
    std::string canon;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        std::string actual;
        json* nxt = child_of(*parent, segs[i], &actual);
        if (!nxt || !is_dir_node(*nxt)) {
            return {false, "No such file or directory in repository: " + internal};
        }
        parent = nxt;
        canon = child_path(canon, actual);
    }

    std::string leaf_actual;
    json* leaf = child_of(*parent, segs.back(), &leaf_actual);
    if (!leaf) return {false, "No such file or directory in repository: " + internal};

    const std::string ipath = child_path(canon, leaf_actual);
    const bool is_dir = is_dir_node(*leaf);
    const std::uint64_t total = count_nodes(*leaf);

    if (is_dir && total > 1 && !recursive) {
        return {false, "'" + ipath + "' is a directory (" + std::to_string(total - 1) +
                           " entries), use rm -r to delete it"};
    }

    std::uint64_t guard = 0;
    if (!free_subtree(*leaf, guard)) return {false, "Cannot free data blocks"};

    std::string bit_err;
    if (!g_bit.save(store::layout().bit, bit_err)) {
        return {false, "Cannot update bit file: " + bit_err};
    }

    (*parent)["children"].erase(leaf_actual);
    if (!save_index(RepoKey(key))) return {false, "Cannot write index"};

    if (total == 1) return {true, "Removed " + ipath};
    return {true, "Removed " + std::to_string(total) + " entries under " + ipath};
}

// ---------------------------------------------------------------- 列表

bool list_files(const std::string& prefix_in, bool recursive, std::vector<ListRow>& out) {
    out.clear();
    json& root = index_tree();

    const std::string internal =
        prefix_in.empty() ? std::string("/") : NormalizeInternalPath(prefix_in);
    if (internal.empty()) return false;

    json* node = &root;
    std::string canon;
    for (const std::string& seg : SplitInternalPath(internal)) {
        std::string actual;
        json* nxt = child_of(*node, seg, &actual);
        if (!nxt) return false;
        node = nxt;
        canon = child_path(canon, actual);
    }
    const std::string ipath = canon.empty() ? "/" : canon;

    if (!is_dir_node(*node)) {
        out.push_back({ipath, entry_size(*node), false});
        return true;
    }

    if (!recursive) {
        std::vector<ListRow> rows;
        if (node->contains("children") && (*node)["children"].is_object()) {
            for (auto it = (*node)["children"].begin(); it != (*node)["children"].end(); ++it) {
                const bool dir = is_dir_node(it.value());
                rows.push_back({child_path(ipath, it.key()),
                                dir ? 0 : entry_size(it.value()), dir});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const ListRow& a, const ListRow& b) {
            if (a.dir != b.dir) return a.dir;
            return a.path < b.path;
        });
        out = std::move(rows);
        return true;
    }

    // 递归：深度优先，目录先于其子项
    std::vector<const json*> stack_dir;
    std::vector<std::string> stack_path;
    stack_dir.push_back(node);
    stack_path.push_back(ipath);
    while (!stack_dir.empty()) {
        const json* cur = stack_dir.back();
        const std::string cpath = stack_path.back();
        stack_dir.pop_back();
        stack_path.pop_back();
        if (!cur->contains("children") || !(*cur)["children"].is_object()) continue;

        std::vector<const json*> child_dirs;
        std::vector<std::string> child_dir_paths;
        std::vector<ListRow> files_here;
        for (auto it = (*cur)["children"].begin(); it != (*cur)["children"].end(); ++it) {
            const std::string p = child_path(cpath, it.key());
            if (is_dir_node(it.value())) {
                out.push_back({p, 0, true});
                child_dirs.push_back(&it.value());
                child_dir_paths.push_back(p);
            } else {
                files_here.push_back({p, entry_size(it.value()), false});
            }
        }
        std::sort(files_here.begin(), files_here.end(),
                  [](const ListRow& a, const ListRow& b) { return a.path < b.path; });
        out.insert(out.end(), files_here.begin(), files_here.end());
        // 逆序入栈，保证输出顺序与名字升序一致
        for (std::size_t i = child_dirs.size(); i > 0; --i) {
            stack_dir.push_back(child_dirs[i - 1]);
            stack_path.push_back(child_dir_paths[i - 1]);
        }
    }
    return true;
}

bool node_kind(const std::string& internal, std::string& kind) {
    kind.clear();
    json& root = index_tree();
    json* node = &root;
    const std::string norm = NormalizeInternalPath(internal);
    if (norm.empty()) return false;
    for (const std::string& seg : SplitInternalPath(norm)) {
        std::string actual;
        json* nxt = child_of(*node, seg, &actual);
        if (!nxt) return false;
        node = nxt;
    }
    kind = node->value("kind", "");
    return !kind.empty();
}

StoreStats stats() {
    StoreStats s;
    if (ensure_bit()) {
        s.blocks = g_bit.cap[store::kTierBig];
        s.blocks_used = g_bit.used(store::kTierBig);
        s.slots = g_bit.cap[store::kTierSmall];
        s.slots_used = g_bit.used(store::kTierSmall);
    }

    json& root = index_tree();
    std::vector<const json*> stack;
    std::vector<bool> is_root;
    stack.push_back(&root);
    is_root.push_back(true);
    std::uint64_t files_count = 0;
    std::uint64_t dirs_count = 0;
    while (!stack.empty()) {
        const json* cur = stack.back();
        const bool top = is_root.back();
        stack.pop_back();
        is_root.pop_back();
        if (is_dir_node(*cur)) {
            if (!top) ++dirs_count;
            if (cur->contains("children") && (*cur)["children"].is_object()) {
                for (auto it = (*cur)["children"].begin(); it != (*cur)["children"].end(); ++it) {
                    stack.push_back(&it.value());
                    is_root.push_back(false);
                }
            }
        } else {
            ++files_count;
            if (cur->value("kind", "") == "inline") ++s.inline_files;
        }
    }
    s.files = files_count;
    s.dirs = dirs_count;
    return s;
}
