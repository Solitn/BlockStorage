#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "json.h"
#include "types.h"

namespace store {

// 固定记录布局：nonce(12) | ciphertext | tag(16)
// 新格式（index v4）：大块 256 KiB 落在 data.blk，小块 4 KiB 落在 data.small
constexpr std::size_t kTinySize   = 1024;             // ≤1 KiB：直接 base64 存索引
constexpr std::size_t kLargeBlock = 256 * 1024;       // 大块载荷 256 KiB
constexpr std::size_t kSmallBlock = 4096;             // 小块载荷 4 KiB
constexpr std::size_t kNonceSize  = 12;
constexpr std::size_t kTagSize    = 16;
constexpr std::size_t kLargeRec   = kNonceSize + kLargeBlock + kTagSize;  // 262172
constexpr std::size_t kSmallRec   = kNonceSize + kSmallBlock + kTagSize;  // 4124

// 旧格式（index ≤ v3）：data.blk 4 KiB 记录，data.small 512 B 记录
constexpr std::size_t kLegacyBlockSize = 4096;
constexpr std::size_t kLegacySmallSize = 512;
constexpr std::size_t kLegacyBlockRec  = kNonceSize + kLegacyBlockSize + kTagSize;  // 4124
constexpr std::size_t kLegacySmallRec  = kNonceSize + kLegacySmallSize + kTagSize;  // 540

struct Layout {
    std::string dir     = ".block";
    std::string index   = ".block/idx";
    std::string key     = ".block/key";
    std::string data    = ".block/data.blk";
    std::string bitmap  = ".block/bitmap";
    std::string small_data   = ".block/data.small";
    std::string small_bitmap = ".block/bitmap.small";
};

const Layout& layout();

bool path_exists(const std::string& path);
bool file_size_of(const std::string& path, std::uint64_t& out);
bool file_size_of(const std::filesystem::path& path, std::uint64_t& out);

// 创建 .block 及全部占位文件
bool create_area(std::string& err);

// 位图
bool bitmap_load(const std::string& path, std::vector<std::uint8_t>& bits);
bool bitmap_save(const std::string& path, const std::vector<std::uint8_t>& bits);
bool bitmap_test(const std::vector<std::uint8_t>& bits, std::uint64_t i);
void bitmap_set(std::vector<std::uint8_t>& bits, std::uint64_t i, bool value);
bool bitmap_first_free(const std::vector<std::uint8_t>& bits, std::uint64_t& out);
std::uint64_t bitmap_used(const std::vector<std::uint8_t>& bits);

// 定长记录
bool record_write(const std::string& path, std::size_t rec_size, std::uint64_t id, const U8list& rec);
bool record_read(const std::string& path, std::size_t rec_size, std::uint64_t id, U8list& rec);
std::uint64_t record_count(const std::string& path, std::size_t rec_size);

}  // namespace store
