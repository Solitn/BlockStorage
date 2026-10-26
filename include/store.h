#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "json.h"
#include "types.h"

namespace store {

// 数据记录布局：nonce(12) | ciphertext | tag(16)
// 索引 v5：大块 256 KiB（data.blk）、小块 4 KiB（data.small）；
// 块间关系（文件链 / 空闲链）全部记在 bit 文件里，索引只存首尾块号。
constexpr std::size_t kTinySize   = 1024;             // ≤1 KiB：直接 base64 存索引
constexpr std::size_t kLargeBlock = 256 * 1024;       // 大块载荷 256 KiB
constexpr std::size_t kSmallBlock = 4096;             // 小块载荷 4 KiB
constexpr std::size_t kNonceSize  = 12;
constexpr std::size_t kTagSize    = 16;
constexpr std::size_t kLargeRec   = kNonceSize + kLargeBlock + kTagSize;  // 262172
constexpr std::size_t kSmallRec   = kNonceSize + kSmallBlock + kTagSize;  // 4124

// bit 表中的两档
constexpr int kTierBig   = 0;
constexpr int kTierSmall = 1;
constexpr int kTierCount = 2;

struct Layout {
    std::string dir   = ".block";
    std::string index = ".block/idx";
    std::string key   = ".block/key";
    std::string data  = ".block/data.blk";
    std::string small = ".block/data.small";
    std::string bit   = ".block/bit";
};

const Layout& layout();

bool path_exists(const std::string& path);
bool file_size_of(const std::string& path, std::uint64_t& out);
bool file_size_of(const std::filesystem::path& path, std::uint64_t& out);

// 创建 .block 及全部占位文件（idx / key / data.blk / data.small / bit）
bool create_area(std::string& err);

// 定长记录
bool record_write(const std::string& path, std::size_t rec_size, std::uint64_t id, const U8list& rec);
bool record_read(const std::string& path, std::size_t rec_size, std::uint64_t id, U8list& rec);
std::uint64_t record_count(const std::string& path, std::size_t rec_size);

// ---------------------------------------------------------------- bit 文件
// 每条目 4 字节：块号 3 字节（小端，0xFFFFFF = 链尾）+ 状态 1 字节（0=空闲，1=占用）。
//   空闲块的后继 = 下一个空闲块；占用块的后继 = 同一文件内的下一个块。
// 文件头保存两档的容量 / 空闲数 / 链首 / 链尾；节点按块号随机读写，
// 因此释放出来的中间块会被直接复用（不要求连续）。
constexpr std::uint32_t kBitMaxBlocks = 0xFFFFFF;  // 每档最多 16,777,215 块（块号 24 位）
constexpr std::int64_t  kBitNone      = -1;

struct BitTable {
    std::uint64_t cap[kTierCount]        = {0, 0};
    std::uint64_t free_count[kTierCount] = {0, 0};
    std::int64_t  head[kTierCount]       = {-1, -1};
    std::int64_t  tail[kTierCount]       = {-1, -1};
    std::vector<std::int32_t> link[kTierCount];   // 后继块号，-1 = 链尾
    std::vector<std::uint8_t> state[kTierCount];  // 0 = 空闲，1 = 占用

    bool load(const std::string& path, std::string& err);
    bool save(const std::string& path, std::string& err) const;

    // 取一个空闲块；空闲链为空时扩展一个块号。失败返回 -1
    std::int64_t alloc(int tier);
    // 归还一个块（追加到空闲链尾）；重复释放返回 false
    bool release(int tier, std::int64_t id);

    std::int64_t next_of(int tier, std::int64_t id) const;
    void set_next(int tier, std::int64_t id, std::int64_t v);
    bool is_used(int tier, std::int64_t id) const;

    std::uint64_t used(int tier) const { return cap[tier] - free_count[tier]; }
    static bool valid_tier(int tier) { return tier >= 0 && tier < kTierCount; }
};

}  // namespace store
