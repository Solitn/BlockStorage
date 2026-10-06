#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstring>
#include <string>
#include <vector>
#include "encrypt.h"
#include "types.h"

std::string Base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};

    // 4 * ceil(n/3) + 1（结尾 '\0'）
    size_t outLen = 4 * ((data.size() + 2) / 3) + 1;
    std::string out(outLen, '\0');

    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            data.data(),
                            static_cast<int>(data.size()));
    if (n < 0) return {};

    out.resize(static_cast<size_t>(n));
    return out;
}
U8list Sha256(const U8list& data) {
    U8list out(32);
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr);
    return out;
}

U8list SlowHash(const U8list& data) {
    static const uint8_t KEY[32] = {
        0xDE,0xAD,0xBE,0xEF, 0xCA,0xFE,0xBA,0xBE,
        0x13,0x37,0xC0,0xDE, 0x42,0x42,0x42,0x42,
        0xFE,0xED,0xFA,0xCE, 0x8B,0xAD,0xF0,0x0D,
        0xDE,0xAD,0xC0,0xDE, 0xBE,0xEF,0x42,0x42
    };

    U8list out(32);
    if (PKCS5_PBKDF2_HMAC(
            reinterpret_cast<const char*>(KEY), sizeof(KEY),
            data.data(), (int)data.size(),
            600000,                 // 迭代次数，越大越慢
            EVP_sha256(),
            32, out.data()) != 1) {
        return {};
    }
    return out;
}
U8list RandomBytes(size_t n) {
    U8list out(n);
    if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        return {};
    }
    return out;
}