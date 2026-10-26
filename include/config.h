#pragma once
#include <string>

// 默认导出根目录：环境变量 BLOCKSTORAGE_OUT > 索引里的 config.output_root > <cwd>\extract
std::string DefaultOutputRoot();

// 解析（不创建）当前导出根目录
std::string OutputRoot();

// 环境变量 BLOCKSTORAGE_OUT 的值（未设置返回空串）
std::string EnvOutputRoot();

// 校验并记住导出根（写入内存索引；需要调用方 save_index 落盘）
bool SetOutputRoot(const std::string& path, std::string& err);

// 确保目录存在
bool EnsureDirectory(const std::string& path, std::string& err);
