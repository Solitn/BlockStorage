#include "files.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "encrypt.h"
#include "main.h"
#include "store.h"
#include "types.h"

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
    j["version"] = 1;
    j["files"] = json::object();
    return j;
}

json& index_root() {
    if (!files.is_object() || !files.contains("files") || !files["files"].is_object()) {
        files = empty_index();
    }
    return files["files"];
}

bool write_all(const std::string& path, const U8list& data) {
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

}  // namespace

U8list RepoKey(const U8list& key) {
    return key.size() == 32 ? key : kDefaultKey;
}

bool check_dir() {
    return store::path_exists(store::layout().dir);
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
    files = j;
    return true;
}

bool save_index(const U8list& key) {
    if (!check_dir()) return false;
    U8list raw = encrypt_json(files, RepoKey(key));
    if (raw.empty()) return false;
    return write_all(store::layout().index, raw);
}

Result add_file(const std::string& src, const std::string& name_in, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};

    std::error_code ec;
    if (!fs::exists(src, ec) || fs::is_directory(src, ec)) {
        return {false, "Source file not found: " + src};
    }
    std::uint64_t src_size = 0;
    if (!store::file_size_of(src, src_size)) return {false, "Cannot stat source file"};

    U8list plain = read_file(src);
    if (plain.size() != src_size) return {false, "Cannot read source file"};

    std::string name = name_in;
    if (name.empty()) name = fs::path(src).filename().string();
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        return {false, "Invalid repository file name"};
    }

    json& idx = index_root();
    if (idx.contains(name)) return {false, "File already exists in repository: " + name};

    const store::Layout& L = store::layout();
    const U8list master = RepoKey(key);
    const std::string digest = Base64Encode(Sha256(plain));

    if (plain.size() <= kSmallSize) {
        std::vector<std::uint8_t> bits;
        if (!store::bitmap_load(L.smbitmap, bits)) return {false, "Cannot read small bitmap"};
        std::uint64_t slot = next_id(L.small, kSmallRec, bits, 0);

        U8list payload(kSmallSize, 0);
        std::copy(plain.begin(), plain.end(), payload.begin());
        U8list rec = GcmSeal(payload, SubKey(master, 2, slot));
        if (rec.empty()) return {false, "Encryption failed"};
        if (!store::record_write(L.small, kSmallRec, slot, rec)) {
            return {false, "Cannot write small slot"};
        }
        store::bitmap_set(bits, slot, true);
        if (!store::bitmap_save(L.smbitmap, bits)) return {false, "Cannot update small bitmap"};

        json entry;
        entry["kind"] = "small";
        entry["slot"] = slot;
        entry["size"] = plain.size();
        entry["sha256"] = digest;
        idx[name] = entry;
        if (!save_index(master)) {
            store::bitmap_set(bits, slot, false);
            store::bitmap_save(L.smbitmap, bits);
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

Result get_file(const std::string& name, const std::string& dst_in, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};
    json& idx = index_root();
    if (!idx.contains(name)) return {false, "No such file in repository: " + name};

    const store::Layout& L = store::layout();
    const U8list master = RepoKey(key);
    const json& entry = idx[name];

    U8list plain;
    if (!entry.contains("kind") || !entry.contains("size")) {
        return {false, "Corrupted index entry: " + name};
    }

    if (entry["kind"] == "small") {
        const std::uint64_t slot = entry["slot"].get<std::uint64_t>();
        U8list rec;
        if (!store::record_read(L.small, kSmallRec, slot, rec)) {
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
        return {false, "Unknown entry kind for " + name};
    }

    const std::uint64_t size = entry["size"].get<std::uint64_t>();
    if (plain.size() < size) return {false, "Truncated data for " + name};
    plain.resize(static_cast<std::size_t>(size));

    if (entry.contains("sha256")) {
        const std::string want = entry["sha256"].get<std::string>();
        if (Base64Encode(Sha256(plain)) != want) {
            return {false, "Checksum mismatch for " + name};
        }
    }

    std::string dst = dst_in.empty() ? name : dst_in;
    fs::path out_path(dst);
    if (out_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
    }
    if (!write_all(dst, plain)) return {false, "Cannot write " + dst};

    return {true, "Extracted " + name + " -> " + dst + " (" + std::to_string(size) + " bytes)"};
}

Result remove_file(const std::string& name, const U8list& key) {
    if (!check_dir()) return {false, "Repository not initialized"};
    json& idx = index_root();
    if (!idx.contains(name)) return {false, "No such file in repository: " + name};

    const store::Layout& L = store::layout();
    const json& entry = idx[name];
    std::string kind = entry.value("kind", "");

    if (kind == "small") {
        std::vector<std::uint8_t> bits;
        if (!store::bitmap_load(L.smbitmap, bits)) return {false, "Cannot read small bitmap"};
        store::bitmap_set(bits, entry["slot"].get<std::uint64_t>(), false);
        if (!store::bitmap_save(L.smbitmap, bits)) return {false, "Cannot update small bitmap"};
    } else if (kind == "blk") {
        std::vector<std::uint8_t> bits;
        if (!store::bitmap_load(L.bitmap, bits)) return {false, "Cannot read bitmap"};
        for (const auto& b : entry["blocks"]) {
            store::bitmap_set(bits, b.get<std::uint64_t>(), false);
        }
        if (!store::bitmap_save(L.bitmap, bits)) return {false, "Cannot update bitmap"};
    } else {
        return {false, "Unknown entry kind for " + name};
    }

    idx.erase(name);
    if (!save_index(RepoKey(key))) return {false, "Cannot write index"};
    return {true, "Removed " + name};
}

bool list_files(std::vector<std::pair<std::string, std::uint64_t>>& out) {
    out.clear();
    json& idx = index_root();
    for (auto it = idx.begin(); it != idx.end(); ++it) {
        std::uint64_t size = 0;
        if (it.value().contains("size")) size = it.value()["size"].get<std::uint64_t>();
        out.emplace_back(it.key(), size);
    }
    std::sort(out.begin(), out.end());
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
    store::bitmap_load(L.smbitmap, bits);
    s.slots_used = store::bitmap_used(bits);
    s.slots = std::max<std::uint64_t>(s.slots_used, store::record_count(L.small, kSmallRec));
    return s;
}
