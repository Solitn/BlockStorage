#include "store.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace store {
namespace {

// bit 文件头：magic(8) | version(4) | node_size(4) | 8 x 8 字段 | 保留(16) = 96
constexpr char kBitMagic[8] = {'B', 'S', 'B', 'I', 'T', '0', '0', '1'};
constexpr std::uint32_t kBitVersion = 1;
constexpr std::uint32_t kBitNodeSize = 8;
constexpr std::size_t kBitHeader = 96;

void put_u32(U8list& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_u64(U8list& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_i64(U8list& out, std::int64_t v) {
    put_u64(out, static_cast<std::uint64_t>(v));
}

std::uint32_t get_u32(const U8list& in, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[off + i]) << (8 * i);
    return v;
}

std::uint64_t get_u64(const U8list& in, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[off + i]) << (8 * i);
    return v;
}

std::int64_t get_i64(const U8list& in, std::size_t off) {
    return static_cast<std::int64_t>(get_u64(in, off));
}

}  // namespace

const Layout& layout() {
    static const Layout inst;
    return inst;
}

bool path_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool file_size_of(const std::string& path, std::uint64_t& out) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return false;
    out = static_cast<std::uint64_t>(sz);
    return true;
}

bool file_size_of(const fs::path& path, std::uint64_t& out) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return false;
    out = static_cast<std::uint64_t>(sz);
    return true;
}

bool create_area(std::string& err) {
    std::error_code ec;
    const Layout& L = layout();
    if (!fs::is_directory(L.dir, ec)) {
        if (!fs::create_directory(L.dir, ec)) {
            err = "cannot create " + L.dir + ": " + ec.message();
            return false;
        }
    }
    for (const std::string& f : {L.index, L.key, L.data, L.small, L.bit}) {
        if (path_exists(f)) continue;
        std::ofstream ofs(f, std::ios::binary);
        if (!ofs) {
            err = "cannot create " + f;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- bit 文件

bool BitTable::load(const std::string& path, std::string& err) {
    *this = BitTable{};
    err.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) return true;  // 文件不存在：空表

    U8list raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.empty()) return true;
    if (raw.size() < kBitHeader) {
        err = "bit file is truncated";
        return false;
    }
    if (std::memcmp(raw.data(), kBitMagic, sizeof(kBitMagic)) != 0) {
        err = "bit file has a bad magic";
        return false;
    }
    if (get_u32(raw, 8) != kBitVersion || get_u32(raw, 12) != kBitNodeSize) {
        err = "unsupported bit file format";
        return false;
    }

    cap[0]        = get_u64(raw, 16);
    free_count[0] = get_u64(raw, 24);
    head[0]       = get_i64(raw, 32);
    tail[0]       = get_i64(raw, 40);
    cap[1]        = get_u64(raw, 48);
    free_count[1] = get_u64(raw, 56);
    head[1]       = get_i64(raw, 64);
    tail[1]       = get_i64(raw, 72);

    std::size_t off = kBitHeader;
    for (int tier = 0; tier < kTierCount; ++tier) {
        if (off > raw.size() || cap[tier] > (raw.size() - off) / kBitNodeSize) {
            err = "bit file is truncated";
            return false;
        }
        nodes[tier].resize(static_cast<std::size_t>(cap[tier]), -1);
        for (std::uint64_t i = 0; i < cap[tier]; ++i) {
            nodes[tier][static_cast<std::size_t>(i)] = get_i64(raw, off);
            off += kBitNodeSize;
        }
        if (free_count[tier] > cap[tier]) {
            err = "bit file has an inconsistent free count";
            return false;
        }
        if ((head[tier] == -1) != (tail[tier] == -1)) {
            err = "bit file has an inconsistent free list";
            return false;
        }
    }
    return true;
}

bool BitTable::save(const std::string& path, std::string& err) const {
    err.clear();
    U8list raw;
    raw.reserve(kBitHeader + static_cast<std::size_t>(cap[0] + cap[1]) * kBitNodeSize);
    raw.insert(raw.end(), kBitMagic, kBitMagic + sizeof(kBitMagic));
    put_u32(raw, kBitVersion);
    put_u32(raw, kBitNodeSize);
    put_u64(raw, cap[0]);
    put_u64(raw, free_count[0]);
    put_i64(raw, head[0]);
    put_i64(raw, tail[0]);
    put_u64(raw, cap[1]);
    put_u64(raw, free_count[1]);
    put_i64(raw, head[1]);
    put_i64(raw, tail[1]);
    raw.resize(kBitHeader, 0);  // 保留字段
    for (int tier = 0; tier < kTierCount; ++tier) {
        for (std::int64_t v : nodes[tier]) put_i64(raw, v);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!out.good()) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}

std::int64_t BitTable::alloc(int tier) {
    if (!valid_tier(tier)) return -1;
    if (head[tier] >= 0 && static_cast<std::uint64_t>(head[tier]) < cap[tier]) {
        const std::int64_t id = head[tier];
        head[tier] = nodes[tier][static_cast<std::size_t>(id)];
        if (head[tier] < 0) tail[tier] = -1;
        if (free_count[tier] > 0) --free_count[tier];
        nodes[tier][static_cast<std::size_t>(id)] = -1;
        return id;
    }
    // 空闲链为空：扩展一个新的块号
    head[tier] = -1;
    tail[tier] = -1;
    const std::int64_t id = static_cast<std::int64_t>(cap[tier]);
    ++cap[tier];
    nodes[tier].push_back(-1);
    return id;
}

bool BitTable::release(int tier, std::int64_t id) {
    if (!valid_tier(tier)) return false;
    if (id < 0 || static_cast<std::uint64_t>(id) >= cap[tier]) return false;
    nodes[tier][static_cast<std::size_t>(id)] = -1;
    if (tail[tier] == -1) {
        head[tier] = id;
        tail[tier] = id;
    } else {
        nodes[tier][static_cast<std::size_t>(tail[tier])] = id;
        tail[tier] = id;
    }
    ++free_count[tier];
    return true;
}

std::int64_t BitTable::next_of(int tier, std::int64_t id) const {
    if (!valid_tier(tier)) return -1;
    if (id < 0 || static_cast<std::uint64_t>(id) >= cap[tier]) return -1;
    return nodes[tier][static_cast<std::size_t>(id)];
}

void BitTable::set_next(int tier, std::int64_t id, std::int64_t v) {
    if (!valid_tier(tier)) return;
    if (id < 0 || static_cast<std::uint64_t>(id) >= cap[tier]) return;
    nodes[tier][static_cast<std::size_t>(id)] = v;
}

// ---------------------------------------------------------------- 定长记录

bool record_write(const std::string& path, std::size_t rec_size, std::uint64_t id,
                  const U8list& rec) {
    if (rec.size() != rec_size) return false;
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        std::ofstream create(path, std::ios::binary);
        if (!create) return false;
        create.close();
        f.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) return false;
    }
    f.seekp(static_cast<std::streamoff>(id * rec_size), std::ios::beg);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
    f.flush();
    return f.good();
}

bool record_read(const std::string& path, std::size_t rec_size, std::uint64_t id, U8list& rec) {
    std::uint64_t total = 0;
    if (!file_size_of(path, total)) return false;
    if ((id + 1) * rec_size > total) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(id * rec_size), std::ios::beg);
    if (!in) return false;
    rec.assign(rec_size, 0);
    in.read(reinterpret_cast<char*>(rec.data()), static_cast<std::streamsize>(rec_size));
    return in.gcount() == static_cast<std::streamsize>(rec_size);
}

std::uint64_t record_count(const std::string& path, std::size_t rec_size) {
    std::uint64_t total = 0;
    if (!file_size_of(path, total) || rec_size == 0) return 0;
    return total / rec_size;
}

}  // namespace store
