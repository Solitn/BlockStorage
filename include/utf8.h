#pragma once
#include <filesystem>
#include <string>

// Windows 下统一用 UTF-8 作为内部字符串编码：
//   命令行参数（宽字符）-> UTF-8
//   UTF-8 -> std::filesystem::path（宽字符）
// POSIX 下路径本身就是 UTF-8 字节，不需要宽字符转换。
#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
#endif

std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& p);

// 判断是否为合法 UTF-8
bool IsValidUtf8(const std::string& s);

// 把 stdin 读到的字节归一化成 UTF-8：
// 已是 UTF-8 则原样返回；否则按系统 ANSI 代码页（GBK）解码再转 UTF-8。
std::string NormalizeInputEncoding(const std::string& raw);
