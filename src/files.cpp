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

namespace fs = std::filesystem;
using store::kBlockRec;
using store::kBlockSize;
using store::kSmallRec;
using store::kSmallSize;

namespace {

// 无密码仓库使用的固定密钥（仅混淆，不提供机密性）
const U8list kDefaultKey = {
    0x5E, 0x1B, 0x9C, 0x47, 0xD2, 0x80, 0x3A, 0xF6,
    0x28, 0xB5, 0x6D, 0xE1, 0x03, 0x97, 0x4C, 0xAA,
    0x71, 0x0F, 0xC8, 0x34, 0x96, 0x5B, 0xE2, 0x19,
    0x8E, 0xD4, 0x27, 0x63, 0xFA, 0x08, 0xB1, 0x4D
};

json empty_index() {
    json j;
    j["version"] = 3;
    j["files"] = json::object();
    return j;
}

json& index_root() {
    if (!files.is_object() || !files.contains("files") || !files["files"].is_object()) {
        files = empty_index();
    }
    if (!files.contains("version") || files["version"].get<int>() < 3) {
        files["version"] = 3;
    }
    return files["files"];
}

// 建立目录条目（kind = "dir"）；已存在则跳过
bool ensure_dir_entry(json& idx, const std::string& path) {
    if (path.empty() || path == "/") return true;
    auto it = idx.find(path);
    if (it != idx.end()) return it->value("kind", "") == "dir";
    json entry;
    entry["kind"] = "dir";
    idx[path] = entry;
    return true;
}

// 为路径的每一级父目录建立条目
void ensure_parent_dirs(json& idx, const std::string& path) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') ensure_dir_entry(idx, path.substr(0, i));
    }
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

std::uint64_t next_id(const std::string& data_path, std::size_t rec_size,
                      const std::vector<std::uint8_t>& bits, std::uint64_t hint) {
    std::uint64_t id = 0;
    if (store::bitmap_first_free(bits, id)) return id;
    return std::max(store::record_count(data_path, rec_size), hint);
}

// 把内部路径拆成段；"/" 返回空 vector
std::vector<std::string> split_path(const std::string& path) {
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

std::string join_path(const std::vector<std::string>& segs, std::size_t begin) {
    std::string out;
    for (std::size_t i = begin; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

// 内部路径 -> 本地文件系统路径（root 为 UTF-8，段按 UTF-8 转宽字符）
fs::path to_fs_path(const std::string& root, const std::string& internal) {
    fs::path p = PathFromUtf8(root);
    for (const std::string& seg : split_path(internal)) p /= Utf8ToWide(seg);
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
    return join_path(segs, 0);
}

bool load_index(const U8list& key) {
    if (!check_dir()) return false;
    U8list raw = read_file(store::layout().index);
    if (raw.empty()) {
        files = empty_index();
        return true;
    }
    json j = decrypt_json(raw, RepoKey(key));
    if (j.is_null() || !j.is_object() || !j.contains("files") || !j["files"].is_object()) {
        return false;
    }

    // v1（扁平名字）-> v2（内部路径）
    json migrated = json::object();
    for (auto it = j["files"].begin(); it != j["files"].end(); ++it) {
        std::string name = it.key();
        if (name.empty() || name[0] != '/') name = "/" + name;
        migrated[name] = it.value();
    }
    j["files"] = migrated;
    j["version"] = 2;
    files = j;
    return true;
}

bool save_index(const U8list& key) {
    if (!check_dir()) return false;
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
        name = (base == "/" ? "" : base) + "/" + base_name;
    } else {
        name = NormalizeInternalPath(internal_in);
    }
    if (name.empty()) return {false, "Invalid internal path"};
    if (name == "/") return {false, "Internal path must not be the repository root"};

    json& idx = index_root();
    if (idx.contains(name)) return {false, "Path already exists in repository: " + name};
    // 同名目录已存在（有子孙条目）
    const std::string as_prefix = name + "/";
    for (auto it = idx.begin(); it != idx.end(); ++it) {
        if (it.key().rfind(as_prefix, 0) == 0) {
            return {false, "Path exists as a directory in repository: " + name};
        }
    }
    // 某一级父路径是文件
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] != '/') continue;
        auto anc = idx.find(name.substr(0, i));
        if (anc != idx.end() && anc->value("kind", "") != "dir") {
            return {false, "Parent path is a file: " + name.substr(0, i)};
        }
    }
    ensure_parent_dirs(idx, name);

    const store::Layout& L = store::layout();
    const U8list master = RepoKey(key);
    const std::string digest = Base64Encode(Sha256(plain));

    if (plain.size() <= kSmallSize) {
        std::vector<std::uint8_t> bits;
        if (!store::bitmap_load(L.small_bitmap, bits)) return {false, "Cannot read small bitmap"};
        std::uint64_t slot = next_id(L.small_data, kSmallRec, bits, 0);

        U8list payload(kSmallSize, 0);
        std::copy(plain.begin(), plain.end(), payload.begin());
        U8list rec = GcmSeal(payload, SubKey(master, 2, slot));
        if (rec.empty()) return {false, "Encryption failed"};
        if (!store::record_write(L.small_data, kSmallRec, slot, rec)) {
            return {false, "Cannot write small slot"};
        }
        store::bitmap_set(bits, slot, true);
        if (!store::bitmap_save(L.small_bitmap, bits)) return {false, "Cannot update small bitmap"};

        json entry;
        entry["kind"] = "small";
        entry["slot"] = slot;
        entry["size"] = plain.size();
        entry["sha256"] = digest;
        idx[name] = entry;
        if (!save_index(master)) {
            store::bitmap_set(bits, slot, false);
            store::bitmap_save(L.small_bitmap, bits);
            return {false, "Cannot write index"};
        }
        return {true, "Added " + name + " (" + std::to_string(plain.size()) + " bytes, 1 slot)"};
    }

    std::vector<std::uint8_t> bits;
    if (!store::bitmap_load(L.bitmap, bits)) return {false, "Cannot read bitmap"};

    const std::uint64_t nblocks = (plain.size() + kBlockSize - 1) / kBlockSize;
    std::vector<std::uint64_t> allocated;
    allocated.reserve(static_cast<std::size_t>(nblocks));
    json blocks = json::array();
    std::uint64_t hint = 0;

    for (std::uint64_t i = 0; i < nblocks; ++i) {
        const std::uint64_t id = next_id(L.data, kBlockRec, bits, hint);
        hint = id + 1;

        U8list payload(kBlockSize, 0);
        const std::uint64_t offset = i * kBlockSize;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBlockSize, plain.size() - offset));
        std::copy(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                  plain.begin() + static_cast<std::ptrdiff_t>(offset + take),
                  payload.begin());

        U8list rec = GcmSeal(payload, SubKey(master, 1, id));
        if (rec.empty()) {
            for (std::uint64_t done : allocated) store::bitmap_set(bits, done, false);
            store::bitmap_save(L.bitmap, bits);
            return {false, "Encryption failed"};
        }
        if (!store::record_write(L.data, kBlockRec, id, rec)) {
            for (std::uint64_t done : allocated) store::bitmap_set(bits, done, false);
            store::bitmap_save(L.bitmap, bits);
            return {false, "Cannot write data block"};
        }
        store::bitmap_set(bits, id, true);
        allocated.push_back(id);
        blocks.push_back(id);
    }

    if (!store::bitmap_save(L.bitmap, bits)) {
        for (std::uint64_t done : allocated) store::bitmap_set(bits, done, false);
        store::bitmap_save(L.bitmap, bits);
        return {false, "Cannot update bitmap"};
    }

    json entry;
    entry["kind"] = "blk";
    entry["size"] = plain.size();
    entry["sha256"] = digest;
    entry["blocks"] = blocks;
    idx[name] = entry;

    if (!save_index(master)) {
        for (std::uint64_t done : allocated) store::bitmap_set(bits, done, false);
        store::bitmap_save(L.bitmap, bits);
        return {false, "Cannot write index"};
    }

    return {true, "Added " + name + " (" + std::to_string(plain.size()) + " bytes, " +
                      std::to_string(nblocks) + " blocks)"};
}

Result add_tree(const std::string& src_dir, const std::string& internal_prefix,
                const U8list& key, int& files_added, int& dirs_added, int& failed) {
    files_added = 0;
    dirs_added = 0;
    failed = 0;
    if (!check_dir()) return {false, "Repository not initialized"};

    const fs::path root = PathFromUtf8(src_dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return {false, "Not a directory: " + src_dir};
    }

    std::string prefix = internal_prefix.empty() ? "/" : NormalizeInternalPath(internal_prefix);
    if (prefix.empty()) return {false, "Invalid internal path"};

    json& idx = index_root();
    // 导入根目录本身也要有条目（支持空目录）
    if (prefix != "/") {
        ensure_dir_entry(idx, prefix);
        ++dirs_added;
    }

    std::vector<fs::path> files;
    std::vector<fs::path> dirs;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            dirs.push_back(it->path());
        } else {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    std::sort(dirs.begin(), dirs.end());

    auto relative_of = [&root](const fs::path& p, std::string& out) -> bool {
        std::error_code rel_ec;
        fs::path rel = fs::relative(p, root, rel_ec);
        if (rel_ec) return false;
        out.clear();
        for (const auto& part : rel) {
            if (!out.empty()) out += '/';
            out += PathToUtf8(part);
        }
        return true;
    };

    const std::string base = (prefix == "/") ? "" : prefix;

    // 先建目录条目（含空目录）
    for (const fs::path& d : dirs) {
        std::string rel;
        if (!relative_of(d, rel)) {
            ++failed;
            continue;
        }
        if (ensure_dir_entry(idx, base + "/" + rel)) {
            ++dirs_added;
        } else {
            ++failed;
        }
    }

    std::string first_error;
    for (const fs::path& p : files) {
        std::string rel;
        if (!relative_of(p, rel)) {
            ++failed;
            continue;
        }
        Result r = add_file(PathToUtf8(p), base + "/" + rel, key);
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

    // 目录条目需要落盘（add_file 内部已保存过索引则这里再保存一次即可幂等）
    if (dirs_added > 0 && !save_index(RepoKey(key))) return {false, "Cannot write index"};

    std::string msg = "Imported " + std::to_string(files_added) + " file(s), " +
                      std::to_string(dirs_added) + " dir(s) into " +
                      (prefix == "/" ? std::string("/") : prefix);
    if (failed) msg += ", " + std::to_string(failed) + " skipped (" + first_error + ")";
    return {true, msg};
}

// ---------------------------------------------------------------- 读取

static Result export_one(const std::string& internal, const fs::path& out_path,
                         const json& entry, const U8list& master,
                         const store::Layout& L) {
    // 目录条目：只建目录
    if (entry.value("kind", "") == "dir") {
        std::error_code dir_ec;
        fs::create_directories(out_path, dir_ec);
        if (dir_ec && !fs::is_directory(out_path)) {
            return {false, "Cannot create directory: " + dir_ec.message()};
        }
        return {true, ""};
    }

    U8list plain;
    if (!entry.contains("kind") || !entry.contains("size")) {
        return {false, "Corrupted index entry: " + internal};
    }

    if (entry["kind"] == "small") {
        const std::uint64_t slot = entry["slot"].get<std::uint64_t>();
        U8list rec;
        if (!store::record_read(L.small_data, kSmallRec, slot, rec)) {
            return {false, "Cannot read small slot " + std::to_string(slot)};
        }
        U8list payload;
        if (!GcmOpen(rec, SubKey(master, 2, slot), payload)) {
            return {false, "Data authentication failed (wrong key or corrupted slot)"};
        }
        plain = payload;
    } else if (entry["kind"] == "blk") {
        for (const auto& b : entry["blocks"]) {
            const std::uint64_t id = b.get<std::uint64_t>();
            U8list rec;
            if (!store::record_read(L.data, kBlockRec, id, rec)) {
                return {false, "Cannot read block " + std::to_string(id)};
            }
            U8list payload;
            if (!GcmOpen(rec, SubKey(master, 1, id), payload)) {
                return {false, "Data authentication failed (wrong key or corrupted block)"};
            }
            plain.insert(plain.end(), payload.begin(), payload.end());
        }
    } else {
        return {false, "Unknown entry kind for " + internal};
    }

    const std::uint64_t size = entry["size"].get<std::uint64_t>();
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

Result get_file(const std::string& internal_in, const std::string& dst_in, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};

    std::string internal = NormalizeInternalPath(internal_in);
    if (internal.empty()) return {false, "Invalid internal path"};

    json& idx = index_root();
    const store::Layout& L = store::layout();
    const U8list master = RepoKey(key);

    const bool has_exact = idx.contains(internal);
    const bool exact_is_dir = has_exact && idx[internal].value("kind", "") == "dir";

    // 1) 精确命中普通文件：导出该文件
    if (has_exact && !exact_is_dir) {
        fs::path out_path;
        if (!dst_in.empty()) {
            const fs::path dst_path = PathFromUtf8(dst_in);
            std::error_code ec;
            if (fs::is_directory(dst_path, ec)) {
                out_path = dst_path / Utf8ToWide(split_path(internal).back());
            } else {
                out_path = dst_path;
            }
        } else {
            out_path = to_fs_path(OutputRoot(), internal);
        }
        Result r = export_one(internal, out_path, idx[internal], master, L);
        if (!r.status) return r;
        const std::uint64_t size = idx[internal]["size"].get<std::uint64_t>();
        return {true, "Extracted " + internal + " -> " + PathToUtf8(out_path) + " (" +
                          std::to_string(size) + " bytes)"};
    }

    // 2) 目录（精确命中的目录条目，或前缀命中的子树）：导出整棵树
    struct Match {
        std::string path;
        const json* entry;
    };
    std::vector<Match> matches;
    const std::string prefix = (internal == "/") ? "/" : internal + "/";
    for (auto it = idx.begin(); it != idx.end(); ++it) {
        if (it.key().rfind(prefix, 0) == 0) matches.push_back({it.key(), &it.value()});
    }
    if (!has_exact && matches.empty()) {
        return {false, "No such file or directory in repository: " + internal};
    }

    const fs::path base = dst_in.empty() ? to_fs_path(OutputRoot(), internal) : PathFromUtf8(dst_in);
    std::string err;
    if (!EnsureDirectory(PathToUtf8(base), err)) {
        return {false, "Cannot create " + PathToUtf8(base) + ": " + err};
    }

    int files = 0;
    int dirs = 0;
    std::string first_error;
    for (const Match& m : matches) {
        const std::string rel = m.path.substr(prefix.size());
        fs::path out = base;
        for (const std::string& seg : split_path(rel)) out /= Utf8ToWide(seg);
        const bool is_dir = m.entry->value("kind", "") == "dir";
        Result r = export_one(m.path, out, *m.entry, master, L);
        if (r.status) {
            if (is_dir) ++dirs; else ++files;
        } else if (first_error.empty()) {
            first_error = r.message;
        }
    }
    if (files == 0 && dirs == 0) {
        if (has_exact) {
            return {true, "Created directory " + PathToUtf8(base)};
        }
        return {false, "Extract failed: " + first_error};
    }

    return {true, "Extracted " + std::to_string(files) + " file(s), " + std::to_string(dirs) +
                      " dir(s) from " + internal + " -> " + PathToUtf8(base)};
}

// ---------------------------------------------------------------- 删除

Result remove_file(const std::string& internal_in, const U8list& key, bool recursive) {
    if (!check_dir()) return {false, "Repository not initialized"};

    std::string internal = NormalizeInternalPath(internal_in);
    if (internal.empty()) return {false, "Invalid internal path"};

    json& idx = index_root();
    const store::Layout& L = store::layout();

    std::vector<std::string> targets;
    if (idx.contains(internal)) {
        const bool is_dir = idx[internal].value("kind", "") == "dir";
        targets.push_back(internal);
        if (is_dir) {
            const std::string prefix = internal + "/";
            for (auto it = idx.begin(); it != idx.end(); ++it) {
                if (it.key().rfind(prefix, 0) == 0) targets.push_back(it.key());
            }
            if (!recursive && targets.size() > 1) {
                return {false, "'" + internal + "' is a directory (" +
                                   std::to_string(targets.size() - 1) +
                                   " entries), use rm -r to delete it"};
            }
        }
    } else {
        const std::string prefix = (internal == "/") ? "/" : internal + "/";
        for (auto it = idx.begin(); it != idx.end(); ++it) {
            if (it.key().rfind(prefix, 0) == 0) targets.push_back(it.key());
        }
        if (targets.empty()) return {false, "No such file or directory in repository: " + internal};
        if (!recursive) {
            return {false, "'" + internal + "' is a directory (" + std::to_string(targets.size()) +
                               " entries), use rm -r to delete it"};
        }
    }

    std::vector<std::uint8_t> bits;
    std::vector<std::uint8_t> small_bits;
    bool blocks_dirty = false;
    bool small_dirty = false;

    for (const std::string& t : targets) {
        const json& entry = idx[t];
        const std::string kind = entry.value("kind", "");
        if (kind == "small") {
            if (small_bits.empty()) store::bitmap_load(L.small_bitmap, small_bits);
            store::bitmap_set(small_bits, entry["slot"].get<std::uint64_t>(), false);
            small_dirty = true;
        } else if (kind == "blk") {
            if (bits.empty()) store::bitmap_load(L.bitmap, bits);
            for (const auto& b : entry["blocks"]) {
                store::bitmap_set(bits, b.get<std::uint64_t>(), false);
            }
            blocks_dirty = true;
        }
    }

    if (blocks_dirty && !store::bitmap_save(L.bitmap, bits)) return {false, "Cannot update bitmap"};
    if (small_dirty && !store::bitmap_save(L.small_bitmap, small_bits)) {
        return {false, "Cannot update small bitmap"};
    }

    for (const std::string& t : targets) idx.erase(t);
    if (!save_index(RepoKey(key))) return {false, "Cannot write index"};

    if (targets.size() == 1) return {true, "Removed " + targets[0]};
    return {true, "Removed " + std::to_string(targets.size()) + " entries under " + internal};
}

// ---------------------------------------------------------------- 列表

bool list_files(const std::string& prefix_in, bool recursive, std::vector<ListRow>& out) {
    out.clear();
    json& idx = index_root();

    std::string prefix = prefix_in.empty() ? "/" : NormalizeInternalPath(prefix_in);
    if (prefix.empty()) return false;

    const bool exact = (prefix != "/" && idx.contains(prefix));
    const bool exact_is_dir = exact && idx[prefix].value("kind", "") == "dir";

    if (exact && !recursive) {
        std::uint64_t size = 0;
        if (idx[prefix].contains("size")) size = idx[prefix]["size"].get<std::uint64_t>();
        out.push_back({prefix, size, exact_is_dir});
        return true;
    }
    if (exact && !exact_is_dir) {
        // 递归列单个文件：只有它自己
        std::uint64_t size = 0;
        if (idx[prefix].contains("size")) size = idx[prefix]["size"].get<std::uint64_t>();
        out.push_back({prefix, size, false});
        return true;
    }

    const std::string base = (prefix == "/") ? "/" : prefix + "/";
    if (exact && exact_is_dir) out.push_back({prefix, 0, true});

    if (recursive) {
        for (auto it = idx.begin(); it != idx.end(); ++it) {
            if (it.key().rfind(base, 0) != 0) continue;
            const bool is_dir = it.value().value("kind", "") == "dir";
            std::uint64_t size = 0;
            if (it.value().contains("size")) size = it.value()["size"].get<std::uint64_t>();
            out.push_back({it.key(), size, is_dir});
        }
    } else {
        std::set<std::string> dirs;
        std::map<std::string, std::uint64_t> plain_files;
        for (auto it = idx.begin(); it != idx.end(); ++it) {
            if (it.key().rfind(base, 0) != 0) continue;
            const std::string rest = it.key().substr(base.size());
            if (rest.empty()) continue;
            const bool is_dir = it.value().value("kind", "") == "dir";
            const std::size_t slash = rest.find('/');
            if (slash != std::string::npos) {
                dirs.insert(base + rest.substr(0, slash));
            } else if (is_dir) {
                dirs.insert(it.key());
            } else {
                std::uint64_t size = 0;
                if (it.value().contains("size")) size = it.value()["size"].get<std::uint64_t>();
                plain_files[rest] = size;
            }
        }
        for (const std::string& d : dirs) out.push_back({d, 0, true});
        for (const auto& kv : plain_files) out.push_back({base + kv.first, kv.second, false});
    }

    std::sort(out.begin(), out.end(), [](const ListRow& a, const ListRow& b) {
        if (a.dir != b.dir) return a.dir;
        return a.path < b.path;
    });
    return true;
}

StoreStats stats() {
    const store::Layout& L = store::layout();
    StoreStats s;
    std::vector<std::uint8_t> bits;

    store::bitmap_load(L.bitmap, bits);
    s.blocks_used = store::bitmap_used(bits);
    s.blocks = std::max<std::uint64_t>(s.blocks_used, store::record_count(L.data, kBlockRec));

    bits.clear();
    store::bitmap_load(L.small_bitmap, bits);
    s.slots_used = store::bitmap_used(bits);
    s.slots = std::max<std::uint64_t>(s.slots_used, store::record_count(L.small_data, kSmallRec));
    return s;
}
