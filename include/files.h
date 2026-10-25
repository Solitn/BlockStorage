#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "json.h"
#include "types.h"

// 仓库/索引
bool check_dir();
bool load_index(const U8list& key);
bool save_index(const U8list& key);
U8list RepoKey(const U8list& key);

// 内部路径："a\\b" / "/a/b/" -> "/a/b"；非法（含 .. ）返回空串；根目录返回 "/"
std::string NormalizeInternalPath(const std::string& raw);

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

struct StoreStats {
    std::uint64_t blocks = 0;
    std::uint64_t blocks_used = 0;
    std::uint64_t slots = 0;
    std::uint64_t slots_used = 0;
};
StoreStats stats();
