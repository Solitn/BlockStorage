#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstring>
#include <string>
#include <vector>
#include "encrypt.h"
#include "types.h"
#include "json.h"
U8list To32(const U8list& in) {
    U8list out(in.begin(), in.begin() + std::min<size_t>(in.size(), 32));
    out.resize(32, 0);
    return out;
}
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

U8list encrypt_json(const nlohmann::json& j,
                                const std::vector<uint8_t>& key) {
    std::string plain = j.dump();

    uint8_t k[32];
    unsigned int klen = 32;
    if (EVP_Digest(key.data(), key.size(), k, &klen,
                EVP_sha256(), nullptr) != 1)
        return {};

    uint8_t nonce[12];
    if (RAND_bytes(nonce, 12) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(),
                        nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, k, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    std::vector<uint8_t> cipher(plain.size() + 16);
    int out_len = 0;

    if (EVP_EncryptUpdate(ctx, cipher.data(), &out_len,
                        reinterpret_cast<const unsigned char*>(plain.data()),
                        (int)plain.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, cipher.data() + out_len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    out_len += final_len;
    cipher.resize(out_len);

    uint8_t tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);

    std::vector<uint8_t> out;
    out.reserve(12 + cipher.size() + 16);
    out.insert(out.end(), nonce,          nonce + 12);
    out.insert(out.end(), cipher.begin(), cipher.end());
    out.insert(out.end(), tag,            tag + 16);
    return out;
}


nlohmann::json decrypt_json(const std::vector<uint8_t>& data,
                            const std::vector<uint8_t>& key) {
    if (data.size() < 12 + 16) return nullptr;

    const uint8_t* nonce = data.data();
    const uint8_t* ct    = data.data() + 12;
    size_t ct_len        = data.size() - 12 - 16;
    const uint8_t* tag   = data.data() + 12 + ct_len;

    uint8_t k[32];
    unsigned int klen = 32;
    if (EVP_Digest(key.data(), key.size(), k, &klen,
                   EVP_sha256(), nullptr) != 1)
        return nullptr;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return nullptr;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(),
                           nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, k, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    std::string plain(ct_len, '\0');
    int out_len = 0;

    if (EVP_DecryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(plain.data()),
                          &out_len, ct, (int)ct_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx,
                            reinterpret_cast<unsigned char*>(plain.data()) + out_len,
                            &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }

    out_len += final_len;
    plain.resize(out_len);

    EVP_CIPHER_CTX_free(ctx);

    return nlohmann::json::parse(plain, nullptr, false);
}