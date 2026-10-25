#include "store.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace store {

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
    for (const std::string& f : {L.index, L.key, L.data, L.small_data, L.bitmap, L.small_bitmap}) {
        if (path_exists(f)) continue;
        std::ofstream ofs(f, std::ios::binary);
        if (!ofs) {
            err = "cannot create " + f;
            return false;
        }
    }
    return true;
}

bool bitmap_load(const std::string& path, std::vector<std::uint8_t>& bits) {
    U8list raw;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            bits.clear();
            return true;  // 不存在视为空位图
        }
        raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bits = raw;
    return true;
}

bool bitmap_save(const std::string& path, const std::vector<std::uint8_t>& bits) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bits.empty()) {
        out.write(reinterpret_cast<const char*>(bits.data()),
                  static_cast<std::streamsize>(bits.size()));
    }
    return out.good();
}

bool bitmap_test(const std::vector<std::uint8_t>& bits, std::uint64_t i) {
    std::uint64_t byte = i / 8;
    if (byte >= bits.size()) return false;
    return (bits[byte] >> (i % 8)) & 0x1u;
}

void bitmap_set(std::vector<std::uint8_t>& bits, std::uint64_t i, bool value) {
    std::uint64_t byte = i / 8;
    if (byte >= bits.size()) bits.resize(static_cast<std::size_t>(byte + 1), 0);
    if (value) {
        bits[byte] = static_cast<std::uint8_t>(bits[byte] | (1u << (i % 8)));
    } else {
        bits[byte] = static_cast<std::uint8_t>(bits[byte] & ~(1u << (i % 8)));
    }
}

bool bitmap_first_free(const std::vector<std::uint8_t>& bits, std::uint64_t& out) {
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(bits.size()) * 8; ++i) {
        if (!bitmap_test(bits, i)) {
            out = i;
            return true;
        }
    }
    return false;
}

std::uint64_t bitmap_used(const std::vector<std::uint8_t>& bits) {
    std::uint64_t n = 0;
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(bits.size()) * 8; ++i) {
        if (bitmap_test(bits, i)) ++n;
    }
    return n;
}

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
