#pragma once
#include "types.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "json.h"
using json = nlohmann::json;
namespace fs = std::filesystem;
bool check_dir();
Result add_file(std::string in_file_name /*内部名称*/ , std::string out_file_name); // 添加文件