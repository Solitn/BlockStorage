#pragma once
#include <string>
#include <vector>
#include "main.h"
#include "types.h"

void cmd_prepare(const Args& args);
Result getStatu(const Args& args);
// 仓库可用且索引已载入；否则返回错误（brow 等命令共用）
Result require_loaded();
Result cmd_statu(const Args& args);
Result cmd_init(const Args& args);
Result cmd_load(const Args& args);
Result cmd_delete(const Args& args);
Result cmd_help(const Args& args);
Result cmd_add(const Args& args);
Result cmd_import(const Args& args);
Result cmd_brow(const Args& args);
Result cmd_get(const Args& args);
Result cmd_ls(const Args& args);
Result cmd_rm(const Args& args);
Result cmd_out(const Args& args);
Result command(const std::string& input);
std::vector<std::string> SplitCommandLine(const std::string& cmd);
