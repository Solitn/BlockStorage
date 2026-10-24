#pragma once
#include <string>
#include <vector>
#include "json.h"
#include "types.h"

namespace store {

// 固定块布局：nonce(12) | ciphertext | tag(16)
constexpr std::size_t kBlockSize = 4096;  // 大块载荷
constexpr std::size_t kSmallSize = 512;   // 小文件槽载荷
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize   = 16;
constexpr std::size_t kBlockRec  = kNonceSize + kBlockSize + kTagSize;  // 4124
constexpr std::size_t kSmallRec  = kNonceSize + kSmallSize + kTagSize;  // 540

struct Layout {
    std::string dir     = ".block";
    std::string index   = ".block/idx";
    std::string key     = ".block/key";
    std::string data    = ".block/data.blk";
    std::string bitmap  = ".block/bitmap";
    std::string small   = ".block/data.small";
    std::string smbitmap= ".block/bitmap.small";
};

const Layout& layout();

bool path_exists(const std::string& path);
bool file_size_of(const std::string& path, std::uint64_t& out);

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
