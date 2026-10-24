#pragma once
#include <string>
#include <vector>
#include "json.h"
#include "types.h"

U8list To32(const U8list& in);
std::string Base64Encode(const std::vector<uint8_t>& data);
U8list SlowHash(const U8list& data);
U8list Sha256(const U8list& data);
U8list RandomBytes(size_t n);

// password ‖ salt -> PBKDF2 派生密钥
U8list DeriveKey(const U8list& password, const U8list& salt);
// 用主密钥为第 id 个记录派生独立子密钥（domain 区分大块/小槽）
U8list SubKey(const U8list& master, uint8_t domain, uint64_t id);

// AES-256-GCM：nonce(12) ‖ ciphertext ‖ tag(16)
U8list GcmSeal(const U8list& plain, const U8list& key);
bool GcmOpen(const U8list& sealed, const U8list& key, U8list& plain);

nlohmann::json decrypt_json(const std::vector<uint8_t>& data,
                            const std::vector<uint8_t>& key);
U8list encrypt_json(const nlohmann::json& j,
                                const std::vector<uint8_t>& key);
