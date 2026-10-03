#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstdint>

#pragma comment(lib, "bcrypt.lib")

namespace encrypt {

    // 1. 字符串加密（确定性，同一 key + 同一明文 → 同一密文）
    std::vector<uint8_t> encrypt_string(const std::vector<uint8_t>& plain,
                                        const std::vector<uint8_t>& key);

    // 2. SHA256
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

    // 3. 文件流加密（长度不变）
    bool encrypt_stream(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const std::vector<uint8_t>& key);

    // 4. 文件流解密（长度不变）
    bool decrypt_stream(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const std::vector<uint8_t>& key);

    // 5. 随机一个字节
    uint8_t random_byte();

}