#pragma once
#include <string>
#include <vector>
#include "main.h"
#include "types.h"
void cmd_prepare(const std::vector<std::string>& args);
Result cmd_delete(const std::vector<std::string>& args);
Result cmd_help(const std::vector<std::string>& args);
Result cmd_init(const std::vector<std::string>& args);
Result command(const std::string& input);