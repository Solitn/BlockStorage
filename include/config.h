#pragma once
#include <string>

// 默认导出根目录：环境变量 BLOCKSTORAGE_OUT > .block/config.json > C:\BlockStorage\extract
std::string DefaultOutputRoot();
std::string ConfigFilePath();

// 解析（不创建）当前导出根目录
std::string OutputRoot();

// 环境变量 BLOCKSTORAGE_OUT 的值（未设置返回空串）
std::string EnvOutputRoot();

// 设置并写回 .block/config.json
bool SetOutputRoot(const std::string& path, std::string& err);

// 确保目录存在
bool EnsureDirectory(const std::string& path, std::string& err);
