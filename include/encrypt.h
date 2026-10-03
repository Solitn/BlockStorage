#pragma once
#include "types.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstdint>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace encrypt {
    std::string encode(const U8list& data);

    // 编码：字符串 -> base64 文本
    std::string encode(const std::string& text);

    // 解码：base64 文本 -> 二进制
    U8list decode(const std::string& text);
    // 1. 字符串加密（确定性，同一 key + 同一明文 → 同一密文）
    U8list encrypt_string(const U8list& plain,
                                        const U8list& key);

    // 2. SHA256
    U8list sha256(const U8list& data);

    // 3. 文件流加密（长度不变）
    bool encrypt_stream(const U8list& input,
                        U8list* output,
                        const U8list& key);

    // 4. 文件流解密（长度不变）
    bool decrypt_stream(const U8list& input,
                        U8list* output,
                        const U8list& key);

    // 5. 随机一个字节
    uint8_t random_byte();

}