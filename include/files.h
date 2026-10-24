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

// 文件操作
Result add_file(const std::string& src, const std::string& name, const U8list& key);
Result get_file(const std::string& name, const std::string& dst, const U8list& key);
Result remove_file(const std::string& name, const U8list& key);
bool list_files(std::vector<std::pair<std::string, std::uint64_t>>& out);

struct StoreStats {
    std::uint64_t blocks = 0;
    std::uint64_t blocks_used = 0;
    std::uint64_t slots = 0;
    std::uint64_t slots_used = 0;
};
StoreStats stats();
