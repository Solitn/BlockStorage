#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "json.h"
#include "types.h"

// 仓库 / 索引
bool check_dir();
bool load_index(const U8list& key);
bool save_index(const U8list& key);
U8list RepoKey(const U8list& key);

// 索引里记录的导出根（空串 = 未设置）；Set 只改内存，需调用方 save_index
std::string IndexOutputRoot();
bool SetIndexOutputRoot(const std::string& path);

// 内部路径："a\\b" / "/a/b/" -> "/a/b"；非法（含 .. ）返回空串；根目录返回 "/"
std::string NormalizeInternalPath(const std::string& raw);
std::vector<std::string> SplitInternalPath(const std::string& path);
std::string JoinInternalPath(const std::string& base, const std::string& rel);

// 文件操作（internal 为仓库内部路径）
Result add_file(const std::string& src, const std::string& internal, const U8list& key);
// 递归导入目录：文件与子目录（含空目录）都会建立索引条目
Result add_tree(const std::string& src_dir, const std::string& internal_prefix,
                const U8list& key, int& files_added, int& dirs_added, int& failed);
Result get_file(const std::string& internal, const std::string& dst, const U8list& key);
Result remove_file(const std::string& internal, const U8list& key, bool recursive);

struct ListRow {
    std::string path;   // 内部路径
    std::uint64_t size = 0;
    bool dir = false;
};
bool list_files(const std::string& prefix, bool recursive, std::vector<ListRow>& out);

// 查询条目类型："dir" / "inline" / "small" / "blk"；不存在返回 false
bool node_kind(const std::string& internal, std::string& kind);

struct StoreStats {
    std::uint64_t blocks = 0;        // 256 KiB 大块总数
    std::uint64_t blocks_used = 0;
    std::uint64_t slots = 0;         // 4 KiB 小块总数
    std::uint64_t slots_used = 0;
    std::uint64_t inline_files = 0;  // 直接存索引的内联文件数
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
};
StoreStats stats();
