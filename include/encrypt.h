#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstdint>

#pragma comment(lib, "bcrypt.lib")

namespace encrypt {

// ============================================================
// 1. 加密字符串
// 输入：plain    - 明文字符串
//       password - 密钥
// 输出：加密后的二进制数据
// ============================================================
std::vector<uint8_t> encrypt_string(const std::string& plain,
                                     const std::string& password);

// ============================================================
// 2. SHA256
// 输入：data - 任意长度二进制数据
// 输出：32 字节哈希值
// ============================================================
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

// ============================================================
// 3. 文件加密（长度不变）
// 输入：input    - 源数据列表（引用，只读）
//       output   - 目标列表（指针，写入）
//       password - 密钥
// 输出：bool，true 成功，false 失败
//       output 长度 == input 长度
// ============================================================
bool encrypt_file(const std::vector<uint8_t>& input,
                  std::vector<uint8_t>* output,
                  const std::string& password);

// ============================================================
// 4. 文件解密（长度不变）
// 输入：input    - 源数据列表（引用，只读）
//       output   - 目标列表（指针，写入）
//       password - 密钥
// 输出：bool，true 成功，false 失败
//       output 长度 == input 长度
// ============================================================
bool decrypt_file(const std::vector<uint8_t>& input,
                  std::vector<uint8_t>* output,
                  const std::string& password);

} // namespace encrypt